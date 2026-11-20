"""Wide->Narrow-Konvertierung (0x2690A4) durch 1:1-Kopie ersetzen.

fn_00130220 konvertiert den Wide-String bei obj+0x118 ueber 0x2690A4 in
einen Narrow-Puffer (max 0x40) und baut daraus das Pfad-Objekt bei
obj+0x138 (0x13A398/0x139854), das 0x13C46C nach Groesse/Offset fragt.

Im Port scheitert 0x2690A4 (gleiche Locale-Ursache wie mblen), daher bleibt
der Puffer leer und 0x13C46C liefert 0x3F800000 (float 1.0) statt der
Dateigroesse.

Fix: Aufruf 0x130270 springt in eine Code-Cave, die jedes u16-Zeichen als
ein Byte kopiert, die Laenge nach [r3] schreibt und 1 zurueckgibt.

  0x130270  bl 0x2690A4  ->  bl 0x399FC0

Idempotent; aendert nur out/exefs/code_decomp.bin.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000

CAVE = 0x00399F00
CALL = 0x00130270

WORDS = [
    (CAVE + 0x00, 0xE3A0C000),   # mov r12,#0
    (CAVE + 0x04, 0xE1D020B0),   # ldrh r2,[r0]
    (CAVE + 0x08, 0xE3520000),   # cmp r2,#0
    (CAVE + 0x0C, 0x0A000005),   # beq done (C+0x28)
    (CAVE + 0x10, 0xE5C12000),   # strb r2,[r1]
    (CAVE + 0x14, 0xE2800002),   # add r0,r0,#2
    (CAVE + 0x18, 0xE2811001),   # add r1,r1,#1
    (CAVE + 0x1C, 0xE28CC001),   # add r12,r12,#1
    (CAVE + 0x20, 0xE35C0040),   # cmp r12,#0x40
    (CAVE + 0x24, 0x1AFFFFF5),   # bne loop (C)
    (CAVE + 0x28, 0xE583C000),   # str r12,[r3]
    (CAVE + 0x2C, 0xE3A00001),   # mov r0,#1
    (CAVE + 0x30, 0xE12FFF1E),   # bx lr
]


def main():
    d = bytearray(open(PATH, 'rb').read())
    pc = CALL + 8
    off = (CAVE - pc) // 4
    bl = 0xEB000000 | (off & 0xFFFFFF)
    for addr, val in [(CALL, bl)] + WORDS:
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
