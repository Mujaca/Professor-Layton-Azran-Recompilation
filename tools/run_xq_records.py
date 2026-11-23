"""menu_logo.xq: 408-Byte-Body als Record-Liste parsen.

Ab Offset 0x20 dekomprimiert Level-5-LZSS 408 Bytes. Diese bestehen aus
17 Records je 0x18 Byte: u32 Hash + 10 u16 Felder. Der 512x256-Atlas
liegt in out/fa_b/menu/s_menu_ani_a.xa (IMGC-Blob 0).

  python tools/run_xq_records.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _imgc

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    d = open(os.path.join(ROOT, "out", "fa_a", "seq", "menu_logo.xq"),
             "rb").read()
    body = _imgc.decomp(d[0x20:])
    n = (len(body) - 4) // 0x18
    print("Body:", len(body), "Bytes, Records:", n)
    for i in range(n):
        off = 4 + i * 0x18
        if off + 0x18 > len(body):
            break
        h = struct.unpack_from("<I", body, off)[0]
        f = struct.unpack_from("<10H", body, off + 4)
        print("%2d off=%04X hash=%08X %s" % (i, off, h,
                                             " ".join("%04X" % x for x in f)))

    return 0


if __name__ == "__main__":
    sys.exit(main())
