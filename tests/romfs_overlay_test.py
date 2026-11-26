"""Baut und startet tests/romfs_overlay_selftest.c (RomFS-/ARC0-Overlay).

  python tests/romfs_overlay_test.py

Nur synthetische Daten (kein Spielinhalt): tools/modkit.py baut ein
Level-3-RomFS mit einem ARC0-Archiv (Tabellen komprimiert wie im Spiel:
Huffman 4 Bit, unkomprimiert, Huffman 8 Bit, LZ10) und drei Mods. Geprueft:

* Level-5-Dekompression in C == Python fuer alle Methoden 0..4,
* virtuelles Level-3: jede Datei per Hash-Kette auffindbar (wie das Spiel
  sucht), Inhalte = Original bzw. Mod, neue Dateien/Verzeichnisse,
  Prioritaet bei Konflikten (hoeher gewinnt, gleich: Name spaeter gewinnt),
* unveraenderte Dateien behalten ihren relativen Datenoffset,
* ARC0-Overlay: ersetzter Eintrag liefert Mod-Inhalt, alle anderen Eintraege
  und die uebrigen Tabellen bleiben bytegleich, Dateitabelle Methode 0,
* Eintrag, den das Archiv nicht hat, wird ignoriert (geloggt),
* ohne wirksame Mod-Datei bleibt das Overlay inaktiv.
"""
import os
import random
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import modkit  # noqa: E402

EXE = os.path.join(ROOT, "tools", "out", "romfs_overlay_selftest.exe")
WORK = os.path.join(ROOT, "tools", "out", "romfs_overlay_test")

FAILS = []


def check(cond, msg):
    print(("[OK]   " if cond else "[FAIL] ") + msg)
    if not cond:
        FAILS.append(msg)


def build():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), "zig.exe")
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    cmd = [zig, "cc", "-std=c11", "-O1", "-Wall",
           "-I", os.path.join(ROOT, "runtime", "kernel"),
           os.path.join(ROOT, "runtime", "kernel", "romfs_overlay.c"),
           os.path.join(ROOT, "tests", "romfs_overlay_selftest.c"),
           "-o", EXE]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print("BUILD-FEHLER:\n" + p.stderr[-3000:])
        sys.exit(1)


def wfile(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def blob(seed, n):
    r = random.Random(seed)
    # wenige Symbole -> Huffman/LZ10 haben etwas zu tun
    return bytes(r.choice(b"abcdefgh\0\x01") for _ in range(n))


def test_decomp():
    r = random.Random(7)
    cases = [b"", b"x", bytes(range(256)) * 3, b"A" * 1000, blob(1, 777),
             bytes(r.randrange(16) for _ in range(513))]
    for m in range(5):
        for i, data in enumerate(cases):
            if m in (2, 3) and len(set(data)) > 16:
                continue        # Test-Kodierer nur fuer kleine Alphabete
            comp = modkit.compress(data, m)
            ok_py = modkit.decompress(comp) == data
            src = os.path.join(WORK, "c%d_%d.bin" % (m, i))
            dst = src + ".out"
            wfile(src, comp)
            p = subprocess.run([EXE, "decomp", src, dst], capture_output=True, text=True)
            ok_c = p.returncode == 0 and open(dst, "rb").read() == data
            if not (ok_py and ok_c):
                check(False, "Level-5 Methode %d Fall %d: py=%s c=%s" % (m, i, ok_py, ok_c))
                return
    check(True, "Level-5-Dekompression C == Python (Methoden 0..4)")


def main():
    build()
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    test_decomp()

    arc_files = {
        "img/a.bin": blob(10, 300),
        "img/b.bin": blob(11, 41),
        "txt/en/01.xs": blob(12, 123),
        "txt/en/02.xs": blob(13, 77),
        "txt/de/01.xs": blob(14, 55),
    }
    arc = modkit.build_arc0(arc_files)
    check(modkit.Arc0(arc).methods == [2, 0, 3, 1], "synthetisches ARC0: Tabellenmethoden wie im Spiel")
    files = {
        "data/arc/test.fa": arc,
        "data/snd/s1.bin": blob(20, 1000),
        "data/snd/s2.bin": blob(21, 17),
        "data/Movie.bin": blob(22, 4096),
        "top.bin": blob(23, 5),
        "empty.bin": b"",
    }
    l3 = modkit.build_romfs(files)
    image = os.path.join(WORK, "romfs_image.bin")
    wfile(image, b"\xAA" * 0x1000 + l3 + b"\xEE" * 0x200)   # Praefix + Schwanz

    orig = modkit.RomFS(image)
    check(all(orig.lookup(p) is not None for p in files), "Python-Builder: Original per Hash auffindbar")

    mods = os.path.join(WORK, "mods")
    # leeres Mods-Verzeichnis -> inaktiv
    os.makedirs(os.path.join(mods, "leer", "romfs"))
    out = os.path.join(WORK, "virt.bin")
    p = subprocess.run([EXE, "build", image, "0x1000", mods, out], capture_output=True, text=True)
    check(p.returncode == 3, "ohne Mod-Datei inaktiv (Exit %d)" % p.returncode)

    new_s1 = blob(30, 333)
    loser = blob(31, 64)
    winner = blob(32, 65)
    new_file = b"neue Datei\n"
    new_entry = blob(33, 999)
    wfile(os.path.join(mods, "a-mod", "mod.ini"), b"name = A\npriority = 1\n")
    wfile(os.path.join(mods, "a-mod", "romfs", "data", "snd", "s1.bin"), new_s1)
    wfile(os.path.join(mods, "a-mod", "romfs", "top.bin"), loser)
    wfile(os.path.join(mods, "a-mod", "romfs", "neu", "tief", "n.txt"), new_file)
    wfile(os.path.join(mods, "b-mod", "mod.ini"), b"[mod]\npriority = 5\n")
    wfile(os.path.join(mods, "b-mod", "romfs", "TOP.BIN"), winner)   # Gross/klein egal
    wfile(os.path.join(mods, "c-mod", "fa", "data", "arc", "test.fa", "txt", "en", "02.xs"), new_entry)
    wfile(os.path.join(mods, "c-mod", "fa", "data", "arc", "test.fa", "txt", "fr", "01.xs"), b"x")
    p = subprocess.run([EXE, "build", image, "0x1000", mods, out], capture_output=True, text=True)
    print(p.stdout, end="")
    check(p.returncode == 0, "Overlay aktiv, Lesen am Stueck == in Stuecken (Exit %d)" % p.returncode)
    if p.returncode != 0:
        return 1
    check("Konflikt" in p.stdout and "b-mod (priority 5) ersetzt a-mod (priority 1)" in p.stdout,
          "Konflikt geloggt, hoehere Prioritaet gewinnt")
    check("txt/fr/01.xs nicht im Archiv" in p.stdout, "unbekannter Archiv-Eintrag geloggt und ignoriert")

    v = modkit.RomFS(out, 0)
    expect = dict(files)
    expect["data/snd/s1.bin"] = new_s1
    expect["top.bin"] = winner
    expect["neu/tief/n.txt"] = new_file
    ok = True
    for path, data in expect.items():
        hit = v.lookup(path)
        if hit is None:
            print("  nicht per Hash gefunden:", path)
            ok = False
            continue
        if path == "data/arc/test.fa":
            continue
        got = v.read_file(path)
        if got != data:
            print("  Inhalt falsch:", path)
            ok = False
    check(ok and len(v.files) == len(expect), "virtuelles Level-3: alle Dateien per Hash, Inhalte korrekt")
    same = all(v.files[pth][0] == orig.files[pth][0] for pth in ("data/snd/s2.bin", "data/Movie.bin", "empty.bin"))
    check(same, "unveraenderte Dateien behalten ihren relativen Datenoffset")
    check(v.data_off % 16 == 0 and all(v.files[pth][0] % 16 == 0 for pth in v.files),
          "Datenoffsets 16-Byte-ausgerichtet")

    varc_blob = v.read_file("data/arc/test.fa")
    varc = modkit.Arc0(varc_blob)
    oarc = modkit.Arc0(arc)
    check(varc.methods == [2, 0, 0, 1], "virtuelles ARC0: Dateitabelle unkomprimiert, Rest unveraendert")
    check(varc_blob[oarc.t1:oarc.t3] == arc[oarc.t1:oarc.t3] and
          varc_blob[varc.tn:varc.td] == arc[oarc.tn:oarc.td] and
          varc_blob[0x18:0x48] == arc[0x18:0x48],
          "ARC0: Kopf (ab 0x18), Verzeichnis-/Hashtabellen und Namen bytegleich")
    ok = True
    for path, data in arc_files.items():
        want = new_entry if path == "txt/en/02.xs" else data
        if varc.read(path) != want:
            print("  ARC0-Eintrag falsch:", path)
            ok = False
    check(ok, "ARC0: ersetzter Eintrag = Mod, uebrige Eintraege unveraendert")
    check([e[1] for e in varc.entries] == [e[1] for e in oarc.entries] and
          [e[0] for e in varc.entries] == [e[0] for e in oarc.entries],
          "ARC0: Eintragsreihenfolge/Namen unveraendert")
    print("%d Fehler" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
