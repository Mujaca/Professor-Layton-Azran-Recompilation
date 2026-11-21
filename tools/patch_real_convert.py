"""0x13A080 (Pfad-Konvertierung + 0x287D10-Parser) nativ laufen lassen.

Bisherige Notbehelfe (widecopy/narrowcopy/mblen/conv_success) werden
zurueckgenommen; stattdessen wird nur die mblen/mbtowc-Schleife ab
0x13A154 durch eine einfache 1:1-Konvertierung ersetzt. Der Rest von
0x13A080 (Flag, 0x287D10-Parser, IO) laeuft damit wieder original.

Cave 0x399F60:
  ldrb r3,[r6],#1 ; cmp r3,#0 ; beq done
  strh r3,[r5] ; add r5,r5,#2 ; b loop
done: strh r3,[r5] ; b 0x13A194

Idempotent; aendert nur out/exefs/code_decomp.bin.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000

RESTORE = [
    (0x0013023C, 0xEB00278F),   # bl 0x13A080
    (0x00130270, 0xEB04E38B),   # bl 0x2690A4
    (0x0013028C, 0x0A000005),   # beq 0x1302A8
    (0x00130308, 0xE3A00000),   # mov r0,#0
    (0x0013A168, 0xE320F000),   # nop
    (0x0013A170, 0xE1A02000),   # mov r2,r0
    (0x0013A0D4, 0xE320F000),
    (0x0013A0DC, 0xE1A02000),
]

CAVE = 0x00399F60
PATCH = [
    (0x0013A154, 0xEA097F81),   # b 0x399F60
    (CAVE + 0x00, 0xE4D63001),  # ldrb r3,[r6],#1
    (CAVE + 0x04, 0xE3530000),  # cmp r3,#0
    (CAVE + 0x08, 0x0A000002),  # beq done
    (CAVE + 0x0C, 0xE1C530B0),  # strh r3,[r5]
    (CAVE + 0x10, 0xE2855002),  # add r5,r5,#2
    (CAVE + 0x14, 0xEAFFFFF9),  # b loop
    (CAVE + 0x18, 0xE1C530B0),  # strh r3,[r5]  (NUL)
    (CAVE + 0x1C, 0xEA068084),  # b 0x13A194
]


def main():
    d = bytearray(open(PATH, 'rb').read())
    for addr, val in RESTORE + PATCH:
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
