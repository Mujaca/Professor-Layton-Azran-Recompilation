"""Diagnose-Cave: Rueckgabewerte von 0x132FF8 und 0x12FC24 speichern.

Patcht:
  0x124A14  b cave1   (nach bl 0x132FF8)  -> r0 nach 0x0CFFF000
  0x124A40  b cave2   (vor cmp r0,#0 nach bl 0x12FC24) -> r0 nach 0x0CFFF004

Beide Cave-Routinen stellen die Originalregister/Flags wieder her und
springen an die jeweilige Folgeadresse zurueck.

Idempotent; aendert nur out/exefs/code_decomp.bin.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000

SCRATCH1 = 0x0CFFF000
SCRATCH2 = 0x0CFFF004
CAVE1 = 0x00399F80
CAVE2 = 0x00399F60


def b_word(addr, target):
    off = (target - (addr + 8)) // 4
    return 0xEA000000 | (off & 0xFFFFFF)


PATCHES = [
    # b cave1 / cave2
    (0x00124A14, b_word(0x00124A14, CAVE1)),
    (0x00124A40, b_word(0x00124A40, CAVE2)),
    # cave1
    (CAVE1 + 0x00, 0xE59F1008),                    # ldr r1,[pc,#8]
    (CAVE1 + 0x04, 0xE5810000),                    # str r0,[r1]
    (CAVE1 + 0x08, 0xE1A04000),                    # mov r4,r0
    (CAVE1 + 0x0C, b_word(CAVE1 + 0x0C, 0x00124A18)),
    (CAVE1 + 0x10, SCRATCH1),
    # cave2
    (CAVE2 + 0x00, 0xE59F1008),                    # ldr r1,[pc,#8]
    (CAVE2 + 0x04, 0xE5810000),                    # str r0,[r1]
    (CAVE2 + 0x08, 0xE3500000),                    # cmp r0,#0
    (CAVE2 + 0x0C, b_word(CAVE2 + 0x0C, 0x00124A44)),
    (CAVE2 + 0x10, SCRATCH2),
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
