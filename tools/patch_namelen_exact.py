"""Datei-Lookup auf exakte Namenslaenge pruefen.

fn_0012E3C verglich bisher nur den Eltern-Offset und eine Laenge, die
faktisch 0 war; dadurch passte auch ein Praefix-Record
('product.bcsarpcm.bcstm' zu 'product.bcsar').

Dieser Patch ersetzt das bedingte `bne 0x212F5C` an 0x212F44 durch einen
Sprung in eine Code-Cave, die
  * den Eltern-Offset erneut prueft,
  * die Record-nameLen (Bytes, [sp+0x24]) mit der erwarteten Laenge
    ([sp+0x134] Zeichen * 2) vergleicht,
und erst dann zum Namensvergleich (0x212F48) springt.

Idempotent; aendert nur out/exefs/code_decomp.bin.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000

CAVE = 0x00399F4C

PATCHES = [
    (0x00212F44, 0xEA062000),   # b 0x399F4C
    (CAVE + 0x00, 0xE59D0150),  # ldr r0,[sp,#0x150]
    (CAVE + 0x04, 0xE5900000),  # ldr r0,[r0]
    (CAVE + 0x08, 0xE59D3108),  # ldr r3,[sp,#0x108]
    (CAVE + 0x0C, 0xE1500003),  # cmp r0,r3
    (CAVE + 0x10, 0x1A000006),  # bne next
    (CAVE + 0x14, 0xE59DC024),  # ldr r12,[sp,#0x24]
    (CAVE + 0x18, 0xE59D2134),  # ldr r2,[sp,#0x134]
    (CAVE + 0x1C, 0xE1A02082),  # mov r2,r2,lsl #1
    (CAVE + 0x20, 0xE15C0002),  # cmp r12,r2
    (CAVE + 0x24, 0x1A000001),  # bne next
    (CAVE + 0x28, 0xE59D2134),  # ldr r2,[sp,#0x134]
    (CAVE + 0x2C, 0xEA09E3F2),  # b 0x212F48
    (CAVE + 0x30, 0xEA09E3F6),  # b 0x212F5C
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
