"""XQ32-Body von menu_logo.xq dekomprimieren.

Der Body ab Offset 0x24 ist ein Level-5-LZSS-Strom (tools/xfsatool/dectool.exe)
und dekomprimiert zu 2049 Bytes; darin steht u.a. der UTF-16-String
"Document".

  python tools/run_xq_parse.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _imgc

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    p = os.path.join(ROOT, "out", "fa_a", "seq", "menu_logo.xq")
    d = open(p, "rb").read()
    out = _imgc.decomp(d[0x24:])
    dst = os.path.join(ROOT, "out", "menu_logo_dec.bin")
    open(dst, "wb").write(out)
    strings = [m.group().decode("utf-16le", "replace")
               for m in re.finditer(rb"(?:[\x20-\x7e]\x00){3,}", out)]
    print("XQ32:", p, len(d), "Bytes")
    print("Body ab 0x24 dekomprimiert:", len(out), "Bytes ->", dst)
    print("UTF-16-Strings:", strings)
    return 0


if __name__ == "__main__":
    sys.exit(main())
