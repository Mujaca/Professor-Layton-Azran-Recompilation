"""Narrow->Wide-Konvertierung im Archiv-Lader durch 1:1-Kopie ersetzen.

fn_0013A080 ruft fuer jedes Zeichen mblen()/mbtowc(). Im Port liefert
mblen() immer -1 (Locale-Tabelle/ctype-Pfad), wodurch die Schleife
0x13A154 endlos rueckwaerts laeuft.

Fix: nur der Aufruf in fn_00130220 (0x13023C) springt in eine Code-Cave,
die jedes Narrow-Byte als Wide-Zeichen kopiert und mit L"\\0" abschliesst.
fn_0034E4D4 (der andere Aufrufer) bleibt unveraendert.

  0x13023C  bl 0x13A080  ->  bl 0x399FA0

Cave 0x399FA0 (Nullbereich, fn_00399808 hat 0 Aufrufer):
  ldrb r3,[r1],#1
  cmp  r3,#0
  strh r3,[r0]
  add  r0,r0,#2
  bne  <loop>
  mov  r0,#1
  bx   lr

Idempotent; aendert nur out/exefs/code_decomp.bin.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000

CAVE = 0x00399FA0
CALL = 0x0013023C

WORDS = [
    (CAVE + 0x00, 0xE4D13001),   # ldrb r3,[r1],#1
    (CAVE + 0x04, 0xE3530000),   # cmp r3,#0
    (CAVE + 0x08, 0xE1C030B0),   # strh r3,[r0]
    (CAVE + 0x0C, 0xE2800002),   # add r0,r0,#2
    (CAVE + 0x10, 0x1AFFFFFA),   # bne CAVE
    (CAVE + 0x14, 0xE3A00001),   # mov r0,#1
    (CAVE + 0x18, 0xE12FFF1E),   # bx lr
]


def main():
    d = bytearray(open(PATH, 'rb').read())

    # bl 0x399FA0 von 0x13023C
    pc = 0x0013023C + 8
    off = (CAVE - pc) // 4
    bl = 0xEB000000 | (off & 0xFFFFFF)

    for addr, val in [(CALL, bl)] + WORDS:
        o = addr - BASE
        old = struct.unpack_from('<I', d, o)[0]
        if old == val:
            print('%08X bereits ok' % addr)
            continue
        struct.pack_into('<I', d, o, val)
        print('%08X: %08X -> %08X' % (addr, old, val))

    open(PATH, 'wb').write(d)


if __name__ == '__main__':
    main()
