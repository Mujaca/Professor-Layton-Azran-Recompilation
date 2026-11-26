"""Modkit: RomFS- und ARC0-(.fa-)Inhalte aus dem eigenen Dump auflisten und
entpacken, Mods erzeugen (docs/MODDING.md, Stufe A).

  python tools/modkit.py romfs-list [--find TEXT]
  python tools/modkit.py romfs-extract <romfs-pfad> [-o datei]
  python tools/modkit.py fa-list <archiv> [--find TEXT]
  python tools/modkit.py fa-extract <archiv> <eintrag> [-o datei] [--xscr]
  python tools/modkit.py make-testmod [--mods mods] [--text TEXT]

<archiv> ist der RomFS-Pfad eines ARC0-Archivs (z. B. lt6/arc/lt6_uk.fa).
Quelle ist immer das lokale RomFS-Abbild out/romfs_image.bin (aus dem
eigenen Dump, python tools/romfs_image.py); --image waehlt ein anderes.
Ergebnisse landen in ignorierten Pfaden (mods/, scratch/), nie im Repo.

Formate:
* Level-3-RomFS: docs/REFERENCE.md "RomFS".
* Level-5-Kompression: u32-Kopf = Rohgroesse << 3 | Methode
  (0 keine, 1 LZ10, 2 Huffman 4 Bit, 3 Huffman 8 Bit, 4 RLE).
* ARC0: Kopf 0x48, Tabellen Verzeichnisse/Verzeichnis-Hash/Dateien/Namen,
  jede einzeln komprimiert (siehe runtime/kernel/romfs_overlay.c).
* XSCR (Text-/Skriptbloecke in den .xs-Eintraegen): Kopf 0x14 Bytes, drei
  Level-5-komprimierte Abschnitte (siehe xscr_bounds).
"""
import argparse
import heapq
import os
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import fa_index  # noqa: E402  Level-5-Dekompression

IMAGE = ROOT / "out" / "romfs_image.bin"
L3_BASE = 0x1000
END = 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Level-5-Kompression

def decompress(buf):
    return fa_index.decompress(bytes(buf))


def l5_method(buf):
    return struct.unpack_from("<I", buf, 0)[0] & 7


def lz10_compress(data):
    """Gierige LZ10-Kodierung passend zu fa_index.lzss_decode."""
    out = bytearray()
    i, n = 0, len(data)
    table = {}
    while i < n:
        flag_pos = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if i >= n:
                break
            best_len, best_back = 0, 0
            if i + 3 <= n:
                for j in reversed(table.get(data[i:i + 3], [])[-64:]):
                    back = i - j
                    if back > 0x1000:
                        break
                    ln = 3
                    while ln < 18 and i + ln < n and data[j + ln] == data[i + ln]:
                        ln += 1
                    if ln > best_len:
                        best_len, best_back = ln, back
                        if ln == 18:
                            break
            if best_len >= 3:
                flags |= 0x80 >> bit
                v = ((best_len - 3) << 12) | (best_back - 1)
                out += bytes([v >> 8, v & 0xFF])
                step = best_len
            else:
                out.append(data[i])
                step = 1
            for k in range(i, i + step):
                if k + 3 <= n:
                    table.setdefault(data[k:k + 3], []).append(k)
            i += step
        out[flag_pos] = flags
    return struct.pack("<I", (len(data) << 3) | 1) + bytes(out)


def rle_compress(data):
    out = bytearray()
    i, n = 0, len(data)
    lit = bytearray()

    def flush():
        while lit:
            chunk = lit[:128]
            del lit[:128]
            out.append(len(chunk) - 1)
            out.extend(chunk)
    while i < n:
        r = 1
        while i + r < n and r < 130 and data[i + r] == data[i]:
            r += 1
        if r >= 3:
            flush()
            out += bytes([0x80 | (r - 3), data[i]])
            i += r
        else:
            lit.append(data[i])
            i += 1
    flush()
    return struct.pack("<I", (len(data) << 3) | 4) + bytes(out)


def huff_compress(data, bits):
    """Huffman (Nintendo-/cuecomp-Baum) passend zu fa_index.huf_decode.
    Huff4: niedriges Nibble zuerst. Baumlayout breitenweise; Fehler, wenn
    ein Kind-Offset 63 uebersteigt (nur fuer kleine Alphabete gedacht)."""
    if bits == 4:
        syms = []
        for b in data:
            syms += [b & 0xF, b >> 4]
    else:
        syms = list(data)
    freq = {}
    for s in syms:
        freq[s] = freq.get(s, 0) + 1
    for extra in range(1 << bits):         # mindestens zwei Blaetter
        if len(freq) >= 2:
            break
        freq.setdefault(extra, 0)
    heap = [(f, k, ("leaf", s)) for k, (s, f) in enumerate(sorted(freq.items()))]
    heapq.heapify(heap)
    k = len(heap)
    while len(heap) > 1:
        f1, _, a = heapq.heappop(heap)
        f2, _, b = heapq.heappop(heap)
        heapq.heappush(heap, (f1 + f2, k, ("node", a, b)))
        k += 1
    root = heap[0][2]
    # Breitenweise Paare vergeben: Paar p liegt bei Byte 2 + 2*p (relativ zum
    # Baumanfang), die Wurzel bei Byte 1 mit "Paarbasis" 0.
    pairs = []          # je Paar (links, rechts)
    codes = {}
    queue = [(root, 0, "")]     # (Knoten, eigene Paarbasis, Code)
    node_byte = {}              # (paarindex, seite) -> Wert, spaeter gefuellt
    root_byte = None
    qi = 0
    while qi < len(queue):
        node, base, code = queue[qi]
        qi += 1
        _, left, right = node
        pidx = len(pairs)
        pairs.append([left, right])
        addr = 2 + 2 * pidx
        off = (addr - base) // 2 - 1
        if off > 0x3F:
            raise ValueError("Huffman-Baum zu breit fuer Offset-Feld")
        val = off | (0x80 if left[0] == "leaf" else 0) | (0x40 if right[0] == "leaf" else 0)
        if base == 0 and root_byte is None:
            root_byte = val
        else:
            node_byte[id(node)] = val
        for side, ch in ((0, left), (1, right)):
            if ch[0] == "leaf":
                codes[ch[1]] = code + str(side)
            else:
                queue.append((ch, addr, code + str(side)))
    table = bytearray([0, root_byte])
    for left, right in pairs:
        for ch in (left, right):
            table.append(ch[1] if ch[0] == "leaf" else node_byte[id(ch)])
    if len(table) % 4 != 0:               # Bitstrom auf 4 Bytes ausrichten
        table += b"\0\0"
    table[0] = len(table) // 2 - 1
    bitstr = "".join(codes[s] for s in syms)
    words = bytearray()
    for i in range(0, len(bitstr), 32):
        chunk = bitstr[i:i + 32].ljust(32, "0")
        words += struct.pack("<I", int(chunk, 2))
    return struct.pack("<I", (len(data) << 3) | (2 if bits == 4 else 3)) + bytes(table) + bytes(words)


def compress(data, method):
    data = bytes(data)
    if method == 0:
        return struct.pack("<I", len(data) << 3) + data
    if method == 1:
        return lz10_compress(data)
    if method in (2, 3):
        return huff_compress(data, 4 if method == 2 else 8)
    if method == 4:
        return rle_compress(data)
    raise ValueError(method)


# ---------------------------------------------------------------------------
# Level-3-RomFS

def romfs_hash(name, parent):
    h = parent ^ 123456789
    raw = name.encode("utf-16-le")
    for c in struct.unpack("<%dH" % (len(raw) // 2), raw):
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= c
    return h


def hash_table_size(n):
    if n < 3:
        return 3
    if n < 19:
        return n | 1
    c = n
    while any(c % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        c += 1
    return c


class RomFS:
    """Level-3-Ebene aus einer Datei (Abbild + Basis) lesen."""

    def __init__(self, path=IMAGE, base=L3_BASE):
        self.f = open(path, "rb")
        self.base = base
        h = self._read(0, 0x28)
        v = struct.unpack("<10I", h)
        if v[0] != 0x28:
            raise SystemExit("kein Level-3-Kopf in %s" % path)
        self.hdr = v
        self.dhash = struct.unpack("<%dI" % (v[2] // 4), self._read(v[1], v[2]))
        self.dmeta = self._read(v[3], v[4])
        self.fhash = struct.unpack("<%dI" % (v[6] // 4), self._read(v[5], v[6]))
        self.fmeta = self._read(v[7], v[8])
        self.data_off = v[9]
        self.files = {}      # pfad -> (data_rel, size, meta_off, parent_meta)
        self.dirs = {}       # pfad -> meta_off
        self._walk(0, "")

    def _read(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)

    def _dname(self, off):
        n = struct.unpack_from("<I", self.dmeta, off + 0x14)[0]
        return self.dmeta[off + 0x18:off + 0x18 + n].decode("utf-16-le")

    def _walk(self, off, path):
        self.dirs[path] = off
        parent, sib, child, f = struct.unpack_from("<4I", self.dmeta, off)
        while f != END:
            fp, fs, doff, size, hn, nl = struct.unpack_from("<IIQQII", self.fmeta, f)
            name = self.fmeta[f + 0x20:f + 0x20 + nl].decode("utf-16-le")
            self.files[path + name] = (doff, size, f, fp)
            f = fs
        while child != END:
            self._walk(child, path + self._dname(child) + "/")
            child = struct.unpack_from("<I", self.dmeta, child + 4)[0]

    def lookup(self, path):
        """Datei wie das Spiel ueber die Hash-Ketten suchen -> (data_rel, size)."""
        parts = path.strip("/").split("/")
        cur = 0
        for d in parts[:-1]:
            b = self.dhash[romfs_hash(d, cur) % len(self.dhash)]
            while b != END:
                if struct.unpack_from("<I", self.dmeta, b)[0] == cur and self._dname(b) == d:
                    break
                b = struct.unpack_from("<I", self.dmeta, b + 0x10)[0]
            if b == END:
                return None
            cur = b
        name = parts[-1]
        b = self.fhash[romfs_hash(name, cur) % len(self.fhash)]
        while b != END:
            fp, fs, doff, size, hn, nl = struct.unpack_from("<IIQQII", self.fmeta, b)
            if fp == cur and self.fmeta[b + 0x20:b + 0x20 + nl].decode("utf-16-le") == name:
                return doff, size
            b = hn
        return None

    def read_file(self, path, off=0, n=None):
        doff, size, _, _ = self.files[path.strip("/")]
        if n is None:
            n = size - off
        return self._read(self.data_off + doff + off, min(n, size - off))


def build_romfs(files):
    """Level-3-Ebene aus {pfad: bytes} bauen (Reihenfolge wie Azahar
    LayeredFS/3dbrew). Fuer synthetische Selbsttests."""
    root = {"name": "", "dirs": [], "files": [], "parent": None}

    def get_dir(parts):
        d = root
        for p in parts:
            for c in d["dirs"]:
                if c["name"] == p:
                    d = c
                    break
            else:
                c = {"name": p, "dirs": [], "files": [], "parent": d}
                d["dirs"].append(c)
                d = c
        return d
    for path, data in files.items():
        parts = path.strip("/").split("/")
        get_dir(parts[:-1])["files"].append({"name": parts[-1], "data": data, "parent": None})
    dlist, flist = [], []
    cur = {"d": 0, "f": 0}

    def nsize(n):
        return (len(n) * 2 + 3) & ~3

    def prep_dir(d):
        d["meta"] = cur["d"]
        dlist.append(d)
        cur["d"] += 0x18 + nsize(d["name"])

    def prep(d):
        for f in d["files"]:
            f["meta"] = cur["f"]
            f["dir"] = d
            flist.append(f)
            cur["f"] += 0x20 + nsize(f["name"])
        for c in d["dirs"]:
            prep_dir(c)
        for c in d["dirs"]:
            prep(c)
    root["parent"] = root
    prep_dir(root)
    prep(root)
    dh = [END] * hash_table_size(len(dlist))
    fh = [END] * hash_table_size(len(flist))
    dmeta = bytearray(cur["d"])
    fmeta = bytearray(cur["f"])
    for d in dlist:
        par = d["parent"]["meta"]
        sib = END
        if d is not root:
            sibs = d["parent"]["dirs"]
            i = sibs.index(d)
            if i + 1 < len(sibs):
                sib = sibs[i + 1]["meta"]
        b = romfs_hash(d["name"], par) % len(dh)
        nm = d["name"].encode("utf-16-le")
        struct.pack_into("<6I", dmeta, d["meta"], par, sib,
                         d["dirs"][0]["meta"] if d["dirs"] else END,
                         d["files"][0]["meta"] if d["files"] else END, dh[b], len(nm))
        dh[b] = d["meta"]
        dmeta[d["meta"] + 0x18:d["meta"] + 0x18 + len(nm)] = nm
    data = bytearray()
    for f in flist:
        par = f["dir"]["meta"]
        sibs = f["dir"]["files"]
        i = sibs.index(f)
        sib = sibs[i + 1]["meta"] if i + 1 < len(sibs) else END
        b = romfs_hash(f["name"], par) % len(fh)
        nm = f["name"].encode("utf-16-le")
        struct.pack_into("<IIQQII", fmeta, f["meta"], par, sib, len(data), len(f["data"]), fh[b], len(nm))
        fh[b] = f["meta"]
        fmeta[f["meta"] + 0x20:f["meta"] + 0x20 + len(nm)] = nm
        data += f["data"]
        data += b"\0" * ((-len(data)) % 16)
    o_dh = 0x28
    o_dm = o_dh + 4 * len(dh)
    o_fh = o_dm + len(dmeta)
    o_fm = o_fh + 4 * len(fh)
    o_data = (o_fm + len(fmeta) + 15) & ~15
    hdr = struct.pack("<10I", 0x28, o_dh, 4 * len(dh), o_dm, len(dmeta), o_fh, 4 * len(fh),
                      o_fm, len(fmeta), o_data)
    out = bytearray(hdr)
    out += struct.pack("<%dI" % len(dh), *dh) + dmeta + struct.pack("<%dI" % len(fh), *fh) + fmeta
    out += b"\0" * (o_data - len(out))
    return bytes(out + data)


# ---------------------------------------------------------------------------
# ARC0

class Arc0:
    """Level-5-ARC0 aus Bytes lesen. entries: Liste (pfad, index, abs_off, size)."""

    def __init__(self, blob):
        self.blob = blob
        if blob[:4] != b"ARC0":
            raise SystemExit("kein ARC0-Archiv")
        self.t1, self.t2, self.t3, self.tn, self.td = struct.unpack_from("<5I", blob, 4)
        self.methods = [l5_method(blob[a:]) for a in (self.t1, self.t2, self.t3, self.tn)]
        dirs = decompress(blob[self.t1:self.t2])
        ents = decompress(blob[self.t3:self.tn])
        self.names = decompress(blob[self.tn:self.td])
        self.raw_ents = ents
        self.entries = []
        for k in range(0, len(dirs) - 19, 20):
            crc, fdi, dc, ffi, fc, fno, dno = struct.unpack_from("<IHHHHII", dirs, k)
            dname = self._cstr(dno)
            for e in range(ffi, ffi + fc):
                ecrc, noff, off, size = struct.unpack_from("<4I", ents, e * 16)
                self.entries.append((dname + self._cstr(fno + noff), e, self.td + off, size))
        self.entries.sort(key=lambda t: t[1])

    def _cstr(self, at):
        end = self.names.index(b"\0", at)
        return self.names[at:end].decode("latin-1")

    def find(self, path):
        for ent in self.entries:
            if ent[0].lower() == path.lower():
                return ent
        return None

    def read(self, path):
        ent = self.find(path)
        if ent is None:
            raise KeyError(path)
        return self.blob[ent[2]:ent[2] + ent[3]]


def build_arc0(files, methods=(2, 0, 3, 1)):
    """ARC0 aus {pfad: bytes} bauen (Layout wie die Spielarchive: Kopf 0x48,
    Verzeichnisse/Dateien nach CRC sortiert, Daten 4-ausgerichtet mit 4
    Bytes Abstand). Nur fuer synthetische Selbsttests."""
    dirs = {}
    for p in files:
        d, _, n = p.rpartition("/")
        dirs.setdefault(d + "/" if d else "", []).append(n)
    names = bytearray(b"\0")
    dent, fent, data = [], [], bytearray()
    order = sorted(dirs, key=lambda d: zlib.crc32(d.encode("latin-1")))
    for d in order:
        dno = len(names)
        names += d.encode("latin-1") + b"\0"
        fno = len(names)
        flist = sorted(dirs[d], key=lambda n: zlib.crc32(n.encode("latin-1")))
        first = len(fent)
        for n in flist:
            noff = len(names) - fno
            names += n.encode("latin-1") + b"\0"
            blob = files[d + n]
            fent.append(struct.pack("<4I", zlib.crc32(n.encode("latin-1")), noff, len(data), len(blob)))
            data += blob
            data += b"\0" * (4 + (-len(data)) % 4)
        dent.append(struct.pack("<IHHHHII", zlib.crc32(d.encode("latin-1")), 0, 0, first,
                                len(flist), fno, dno))
    dhash = b"".join(struct.pack("<I", zlib.crc32(d.encode("latin-1"))) for d in order)
    tabs = [compress(b"".join(dent), methods[0]), compress(dhash, methods[1]),
            compress(b"".join(fent), methods[2]), compress(bytes(names), methods[3])]
    offs, cur = [], 0x48
    for t in tabs:
        offs.append(cur)
        cur += (len(t) + 3) & ~3
    hdr = bytearray(0x48)
    hdr[:4] = b"ARC0"
    struct.pack_into("<5I", hdr, 4, offs[0], offs[1], offs[2], offs[3], cur)
    struct.pack_into("<HHI", hdr, 0x18, len(dent), len(dent), len(fent))
    out = bytearray(hdr)
    for t in tabs:
        out += t + b"\0" * ((-len(t)) % 4)
    return bytes(out + data)


# ---------------------------------------------------------------------------
# XSCR (.xs): Kopf mit komprimierten Abschnitten

def xscr_bounds(blob):
    """Abschnittsgrenzen eines XSCR-Blocks. Kopf 0x14 Bytes: 'XSCR', u16,
    u16, u32, u32 Beginn Abschnitt 1 / 4, u32 Beginn Abschnitt 2 / 4;
    Abschnitt 0 beginnt bei 0x14, Abschnitt 2 reicht bis zum Ende (geprueft
    an allen 1318 .xs-Eintraegen der drei Archive: jeder Abschnitt ist genau ein
    Level-5-Block)."""
    if blob[:4] != b"XSCR":
        raise ValueError("kein XSCR")
    s1, s2 = struct.unpack_from("<2I", blob, 0x0C)
    return [0x14, s1 * 4, s2 * 4, len(blob)]


def xscr_sections(blob):
    """[(abs_off, komprimierte Laenge, Methode, Rohdaten)] der drei Abschnitte."""
    st = xscr_bounds(blob)
    out = []
    for i in range(3):
        seg = blob[st[i]:st[i + 1]]
        out.append((st[i], len(seg), l5_method(seg), decompress(seg)))
    return out


def xscr_replace_last(blob, raw, method=0):
    """Letzten Abschnitt (Texte) ersetzen; die Offsets im Kopf zeigen nur auf
    die Abschnitte 1 und 2, daher bleibt der Kopf gueltig."""
    st = xscr_bounds(blob)
    return bytes(blob[:st[2]]) + compress(raw, method)


# ---------------------------------------------------------------------------
# Test-Mod (Rezept; Spielinhalt kommt nur aus dem eigenen Dump)

# Erster Dialog nach dem Laden des Test-Spielstands (Szene `froenborg`,
# tools/scene.py, Gast-Frame ~4450): Textblock dieses Eintrags, erster
# sichtbarer Textlauf im Textabschnitt (Abschnitt 2) des XSCR-Blocks.
TESTMOD_ARCHIVE = "lt6/arc/lt6_uk.fa"
TESTMOD_ENTRY = "txt/uk/01/01_010020.xs"
TESTMOD_TEXT = "** MODKIT TEST **"


def first_text_run(raw):
    """(start, ende) des ersten sichtbaren Textlaufs: beginnt nach einem
    Steuer-Tag '>' mit einem druckbaren Zeichen, endet vor dem naechsten '<'
    oder Zeilenumbruch."""
    i = 0
    while True:
        i = raw.find(b">", i)
        if i < 0:
            raise ValueError("kein Textlauf gefunden")
        i += 1
        if i < len(raw) and 0x20 < raw[i] < 0x7F and raw[i] != ord("<"):
            e = i
            while e < len(raw) and raw[e] not in b"<\r\n\x00":
                e += 1
            return i, e


def make_testmod(image, mods, text=TESTMOD_TEXT, control=False, name="testmod-dialog"):
    """Schreibt <mods>/<name>/ mit dem ersetzten Archiv-Eintrag. Der
    Textabschnitt wird unkomprimiert (Methode 0, kommt im Original vor)
    abgelegt; control=True behaelt den Originaltext (Kontrolle fuer den
    Bildvergleich: gleiches Layout, nur der Text unterscheidet sich)."""
    arc = Arc0(RomFS(image).read_file(TESTMOD_ARCHIVE))
    blob = arc.read(TESTMOD_ENTRY)
    raw = bytearray(xscr_sections(blob)[2][3])
    s, e = first_text_run(raw)
    if not control:
        new = text.encode("ascii")[:e - s].ljust(e - s, b" ")
        raw[s:e] = new
    out = Path(mods) / name
    entry = out / "fa" / TESTMOD_ARCHIVE / TESTMOD_ENTRY
    entry.parent.mkdir(parents=True, exist_ok=True)
    entry.write_bytes(xscr_replace_last(blob, bytes(raw), 0))
    (out / "mod.ini").write_text(
        "# erzeugt von tools/modkit.py make-testmod (nicht versionieren)\n"
        "name = %s\nversion = 1\npriority = 0\n" % name, encoding="ascii")
    print("%s: %s/%s, Textlauf 0x%X..0x%X (%d Zeichen)%s" % (
        out, TESTMOD_ARCHIVE, TESTMOD_ENTRY, s, e, e - s, " [Kontrolle]" if control else ""))
    return out


# ---------------------------------------------------------------------------
# CLI

def cmd_romfs_list(a):
    r = RomFS(a.image)
    for p, (doff, size, _, _) in sorted(r.files.items()):
        if a.find and a.find not in p:
            continue
        print("%-48s off=0x%09X size=0x%X" % (p, r.data_off + doff, size))


def write_out(data, out):
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    Path(out).write_bytes(data)
    print("%s (%d Bytes)" % (out, len(data)))


def cmd_romfs_extract(a):
    r = RomFS(a.image)
    write_out(r.read_file(a.path), a.out or (ROOT / "scratch" / "modkit" / "romfs" / a.path))


def open_arc(a):
    return Arc0(RomFS(a.image).read_file(a.archive))


def cmd_fa_list(a):
    arc = open_arc(a)
    print("Tabellen-Methoden (Verz./Hash/Dateien/Namen): %s" % arc.methods)
    for path, idx, off, size in arc.entries:
        if a.find and a.find not in path:
            continue
        print("%-48s #%-5d off=0x%08X size=0x%X" % (path, idx, off, size))


def cmd_fa_extract(a):
    arc = open_arc(a)
    data = arc.read(a.entry)
    out = a.out or (ROOT / "scratch" / "modkit" / "fa" / a.archive / a.entry)
    write_out(data, out)
    if a.xscr:
        for i, (off, clen, meth, raw) in enumerate(xscr_sections(data)):
            write_out(raw, str(out) + ".sec%d" % i)
            print("  Abschnitt %d: off=0x%X komprimiert=%d Methode=%d roh=%d" % (i, off, clen, meth, len(raw)))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", default=str(IMAGE), help="RomFS-Abbild (Standard out/romfs_image.bin)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("romfs-list")
    s.add_argument("--find", default="")
    s.set_defaults(fn=cmd_romfs_list)
    s = sub.add_parser("romfs-extract")
    s.add_argument("path")
    s.add_argument("-o", "--out")
    s.set_defaults(fn=cmd_romfs_extract)
    s = sub.add_parser("fa-list")
    s.add_argument("archive")
    s.add_argument("--find", default="")
    s.set_defaults(fn=cmd_fa_list)
    s = sub.add_parser("fa-extract")
    s.add_argument("archive")
    s.add_argument("entry")
    s.add_argument("-o", "--out")
    s.add_argument("--xscr", action="store_true", help="XSCR-Abschnitte zusaetzlich entpacken")
    s.set_defaults(fn=cmd_fa_extract)
    s = sub.add_parser("make-testmod", help="Test-Mod: ersten Dialog des Test-Spielstands aendern")
    s.add_argument("--mods", default=str(ROOT / "mods"), help="Zielordner (Standard mods/)")
    s.add_argument("--text", default=TESTMOD_TEXT)
    s.add_argument("--control", action="store_true",
                   help="Kontroll-Mod: gleiches Layout, Originaltext")
    s.add_argument("--name", default="")
    s.set_defaults(fn=lambda a: make_testmod(a.image, a.mods, a.text, a.control,
                                             a.name or ("testmod-control" if a.control else "testmod-dialog")) and 0)
    a = ap.parse_args(argv)
    return a.fn(a) or 0


if __name__ == "__main__":
    sys.exit(main())
