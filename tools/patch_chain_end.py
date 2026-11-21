"""Hashketten-Ende korrekt erkennen (0xFFFFFFFF).

Der Dir-/File-Lookup laeuft die Kette ueber `hashnext` ab und beendet sie
an 0x212DF0 mit `cmp r4,#0`. Im RomFS ist das Kettenende aber 0xFFFFFFFF;
dadurch las die Engine mit r4=0xFFFFFFFF weiter und drehte endlos
(Watchdog: 0x212D3C/0x212DA0, pc-Spin nach dem Mount).

Fix (nur Dir-Variante 0x212DEC..0x212DF4):
  0x212DF0  cmp r4,#0    -> cmn r4,#1     (Ende = 0xFFFFFFFF)
  0x212DF4  bne 0x212D3C -> unveraendert (Originalwert wiederherstellen)

Idempotent; aendert nur out/exefs/code_decomp.bin.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000

PATCHES = [
    (0x00212DF0, 0xE3740001),   # cmn r4,#1
    (0x00212DF4, 0x1AFFFFD0),   # bne 0x212D3C
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
