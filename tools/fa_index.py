"""ARC0-.fa-Index lesen: Name -> Datei-Offset.

Format (tools/xfsatool/arc.cpp):
  header: magic, table1, table2, table3, filenameTable, dataOffset
  table3: komprimierte FileEntryArc[] {crc32, unknown, offset, filesize}
  filenameTable: komprimierte NUL-getrennte Namen (Ordner enden auf '/')
  Datei-Offset = dataOffset + entry.offset

  python tools\\fa_index.py out\\romfs\\lt6\\arc\\lt6_a.fa --find menu_logo.xq
"""
import argparse
import struct
import sys
import zlib

LZS_MASK = 0x80
LZS_THRESHOLD = 2


def lzss_decode(buf, raw_len):
    """Level-5 LZSS (cuecomp/lzss.cpp) fuer einen gepufferten Block."""
    if len(buf) < 4:
        return b""
    out = bytearray()
    i = 4
    flags = 0
    mask = 0
    while len(out) < raw_len and i < len(buf):
        mask >>= 1
        if mask == 0:
            flags = buf[i]
            i += 1
            mask = LZS_MASK
        if not (flags & mask):
            if i >= len(buf):
                break
            out.append(buf[i])
            i += 1
        else:
            if i + 1 >= len(buf):
                break
            pos = (buf[i] << 8) | buf[i + 1]
            i += 2
            length = (pos >> 12) + LZS_THRESHOLD + 1
            back = (pos & 0xFFF) + 1
            for _ in range(length):
                if len(out) >= raw_len:
                    break
                out.append(out[-back])
    return bytes(out[:raw_len])


def huf_decode(buf, raw_len, num_bits):
    """Level-5 Huffman (cuecomp/huffman.cpp, HUF_Decode)."""
    if len(buf) < 5:
        return b""
    pak = 4
    tree = pak
    pak += (buf[tree] + 1) * 2
    out = bytearray()
    pos = buf[tree + 1]
    nxt = 0
    mask4 = 0
    code = 0
    cur = 0
    nbits = 0
    while len(out) < raw_len:
        mask4 >>= 1
        if mask4 == 0:
            if pak + 4 > len(buf):
                break
            code = struct.unpack_from("<I", buf, pak)[0]
            pak += 4
            mask4 = 0x80000000
        nxt += ((pos & 0x3F) + 1) * 2
        if not (code & mask4):
            ch = pos & 0x80
            pos = buf[tree + nxt] if tree + nxt < len(buf) else 0
        else:
            ch = pos & 0x40
            pos = buf[tree + nxt + 1] if tree + nxt + 1 < len(buf) else 0
        if ch:
            cur = ((cur << num_bits) | pos) & 0xFF
            nbits = (nbits + num_bits) & 7
            if nbits == 0:
                out.append(cur)
                cur = 0
            pos = buf[tree + 1]
            nxt = 0
    return bytes(out[:raw_len])


def decompress(buf):
    if len(buf) < 5:
        return b""
    size = struct.unpack_from("<I", buf, 0)[0] // 8
    if len(buf) - 4 == size:
        return buf[4:]
    if buf[4] in (0x0F, 0xFF):
        return huf_decode(buf, size, 4 if buf[4] == 0x0F else 8)
    return lzss_decode(buf, size)


def parse(path):
    d = open(path, "rb").read()
    magic, t1, t2, t3, names_off, data_off = struct.unpack_from("<6I", d, 0)
    if magic != 0x30435241:
        raise SystemExit("kein ARC0: %s" % path)
    names = decompress(d[names_off:data_off])
    table = decompress(d[t3:names_off])
    toc = {}
    for i in range(0, len(table) - 15, 16):
        crc, _unk, off, size = struct.unpack_from("<4I", table, i)
        toc[crc] = (data_off + off, size)
    files, folders = [], []
    cur = ""
    for name in names.split(b"\0"):
        if not name:
            continue
        text = name.decode("latin-1")
        if text.endswith("/"):
            folders.append(text)
            cur = text
            continue
        files.append((cur + text, text, zlib.crc32(name) & 0xFFFFFFFF))
    return d, data_off, toc, files, folders


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("archive")
    ap.add_argument("--find")
    ap.add_argument("--limit", type=int, default=12)
    ap.add_argument("--verify", action="store_true",
                    help="Offset-Inhalt gegen .xq-Magic pruefen")
    args = ap.parse_args()
    d, data_off, toc, files, folders = parse(args.archive)
    print("data=0x%X entries=%d names=%d folders=%d"
          % (data_off, len(toc), len(files), len(folders)))
    shown = 0
    for full, base, crc in files:
        if args.find and args.find not in full:
            continue
        hit = toc.get(crc)
        if hit is None:
            continue
        off, size = hit
        note = ""
        if args.verify and size >= 4:
            note = " magic=%s" % d[off:off + 4].hex()
        print("%-42s off=0x%08X size=0x%X%s" % (full, off, size, note))
        shown += 1
        if shown >= args.limit:
            break
    if args.find and shown == 0:
        print("nicht gefunden:", args.find)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
