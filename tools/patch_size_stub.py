"""Diagnose: 0x13C46C (Groessen-Accessor) fest auf die BCSAR-Groesse setzen.

Der Variant-Accessor erwartet an Archiv+0x2DC ein Objekt mit Tag 0x2001
und liefert sonst Muell (float 0x3F800000). Fuer den ersten Ladevorgang
(product.bcsar FF, dataSize 0x00F3DE78) wird der Wert direkt geliefert.

  0x13C46C  ->  b 0x399F40
  Cave: ldr r0,[pc]; bx lr; .word 0x00F3DE78

Idempotent; aendert nur out/exefs/code_decomp.bin.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000

CAVE = 0x00399F40
SIZE = 0x00F3DE78


def main():
    d = bytearray(open(PATH, 'rb').read())
    bpc = 0x0013C46C + 8
    off = (CAVE - bpc) // 4
    b = 0xEA000000 | (off & 0xFFFFFF)
    words = [
        (0x0013C46C, b),
        (CAVE + 0x0, 0xE59F0000),   # ldr r0,[pc]
        (CAVE + 0x4, 0xE12FFF1E),   # bx lr
        (CAVE + 0x8, SIZE),
    ]
    for addr, val in words:
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
