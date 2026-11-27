"""Erzeugt out/exefs/code_decomp.bin = unverändertes code.bin (dekomprimiert)
+ die Patches aus tools/workarounds/code_patches.json.

  python tools/workarounds/apply.py            # alle Patches
  python tools/workarounds/apply.py --none     # unverändertes Original
  python tools/workarounds/apply.py --skip 0x003CBEFC,0x0010F8C4

Jeder Patch prüft die Originalbytes; passt etwas nicht, bricht das Skript ab.
Die Liste ist technische Schuld (docs/WORKAROUNDS.md) und soll schrumpfen.
"""
import argparse
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASE = 0x100000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--none", action="store_true", help="keine Patches anwenden")
    ap.add_argument("--skip", default="", help="Adressen (Komma-getrennt) auslassen")
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "exefs", "code_decomp.bin"))
    ap.add_argument("--code", default=os.path.join(ROOT, "out", "exefs", "code.bin"),
                    help="komprimiertes code.bin (Builder: anderer out-Ordner)")
    a = ap.parse_args()

    from pyctr.type.exefs import decompress_code
    raw = open(a.code, "rb").read()
    code = bytearray(decompress_code(raw))

    skip = {int(x, 16) for x in a.skip.split(",") if x.strip()}
    table = json.load(open(os.path.join(os.path.dirname(__file__), "code_patches.json"),
                           encoding="utf-8"))["patches"]
    applied = 0
    if not a.none:
        for p in table:
            addr = int(p["addr"], 16)
            if addr in skip:
                print("skip  %s  %s" % (p["addr"], p["what"]))
                continue
            for i, (o, n) in enumerate(zip(p["orig"], p["new"])):
                off = addr - BASE + 4 * i
                cur = struct.unpack_from("<I", code, off)[0]
                if cur != int(o, 16):
                    sys.exit("FEHLER %08X: erwartet %s, gefunden %08X" % (addr + 4 * i, o, cur))
                struct.pack_into("<I", code, off, int(n, 16))
            applied += 1
    open(a.out, "wb").write(code)
    print("code_decomp.bin: %d/%d Patches angewendet -> %s" % (applied, len(table), a.out))


if __name__ == "__main__":
    main()
