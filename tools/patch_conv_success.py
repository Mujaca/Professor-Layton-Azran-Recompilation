"""Erfolgspfad von fn_00130220 erzwingen (Pfad-Objekt aufbauen).

fn_00130220 baut nach der Konvertierung das Objekt bei obj+0x138 auf
(0x13A398 + 0x139854) und kehrt ueber 0x1302F0 mit r0=1 zurueck - genau
das braucht fn_0012493C, damit 0x13C46C die Dateigroesse liefert.

Im Port scheitert die Wide->Narrow-Konvertierung 0x2690A4, daher wird
0x1302A8 nie erreicht. Fix: 0x13028C unbedingt nach 0x1302A8 springen.

  0x13028C  beq 0x1302A8  ->  b 0x1302A8
  0x130308  mov r0,#0     ->  mov r0,#1   (Sicherheitsnetz)

Idempotent; aendert nur out/exefs/code_decomp.bin.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000

PATCHES = [
    (0x0013028C, 0xEA000005),   # b 0x1302A8
    (0x00130308, 0xE3A00001),   # mov r0,#1
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
