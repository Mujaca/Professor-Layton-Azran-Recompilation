"""Echten (nicht kurzgeschlossenen) Archiv-Loader lauffaehig machen.

Voraussetzung: `python tools/decomp.py` + `python tools/apply_all_patches.py`.

Dieses Skript nimmt den 0x1249CC-Short-Circuit (patch_bufreturn) wieder
zurueck und setzt die vier Stellen, die der echte Pfad fuer den
product.bcsar-Mount zusaetzlich braucht:

  0x1249CC  b 0x124C30   ->  add r0,r6,#0x2DC   (Original)
  0x28F33C  mov r8,r3                (Copy meldet "size konsumiert")
  0x28F340  beq 0x28F39C -> b 0x28F39C
  0x1301C0  beq 0x1301FC  ->  nop    (consumed-Check ueberspringen)
  0x12FD30  mov r0,#1 ; bx lr        (0x139914-Ergebnis als Erfolg)

Zusaetzlich muessen im Port diese Diagnose-Schalter aktiv sein (Runtime,
keine Code-Patches):  HLE_OBJ_FIX=1 HLE_SPAN_FIX=1 HLE_REG_FIX=1
sowie HLE_FILE_PRELOAD=1 und HLE_BIGARENA=1.

Idempotent; aendert nur out/exefs/code_decomp.bin.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000

PATCHES = [
    (0x001249CC, 0xE2860FB7),   # add r0,r6,#0x2DC   (Original, kein Short-Circuit)
    (0x0028F33C, 0xE1A08003),   # mov r8,r3
    (0x0028F340, 0xEA000015),   # b 0x28F39C
    (0x001301C0, 0xE1A00000),   # nop (consumed-Check)
    (0x0012FD30, 0xE3A00001),   # mov r0,#1
    (0x0012FD34, 0xE12FFF1E),   # bx lr
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
