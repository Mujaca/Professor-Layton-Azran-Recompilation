"""Datei-Lader 0x1300C8 auf vorab geladenen Puffer umbiegen.

HLE_FILE_PRELOAD fuellt den Zielpuffer (r1) bereits mit dem BCSAR-Inhalt
(aus out/romfs_image.bin). Dann genuegt:

  0x1300C8  mov r0,r1 ; bx lr     -> Puffer als Erfolg zurueckgeben
  0x1249CC  b 0x124C30            -> fn_0012493C gibt r0 (Puffer) zurueck

Idempotent; aendert nur out/exefs/code_decomp.bin.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000

PATCHES = [
    (0x001300C8, 0xE1A00001),   # mov r0,r1
    (0x001300CC, 0xE12FFF1E),   # bx lr
    (0x001249CC, 0xEA000097),   # b 0x124C30
]


def main():
    d = bytearray(open(PATH, 'rb').read())
    for addr, val in PATCHES:
        o = addr - BASE
        cur = struct.unpack_from('<I', d, o)[0]
        if cur == val:
            print('%08X bereits ok' % addr)
            continue
        struct.pack_into('<I', d, o, val)
        print('%08X: %08X -> %08X' % (addr, cur, val))
    open(PATH, 'wb').write(d)


if __name__ == '__main__':
    main()
