"""Diagnose-Patch: Endlos-Unlink in fn_00274C98 vermeiden.

fn_00274C98 (intrusive Listen-Unlink) hat diese Schleife:

    0x274CA8  ldr  r1, [r0]        ; Knoten
    0x274CAC  ldr  r3, [r1]        ; Knoten->next
    0x274CB0  cmp  r3, r0
    0x274CB4  bne  0x274D08        ; bei Mismatch: KEIN Dekrement
    ...
    0x274D00  sub  r3, r1, #1
    0x274D04  str  r3, [r0, #8]    ; Zaehler [r0+8] nur im Erfolgsfall
    0x274D08  ldr  r1, [r0, #8]
    0x274D10  bne  0x274CA8

Ist der Listenknoten nicht mit dem Anker verkettet, laeuft die Schleife
endlos (gemessen: >1,4 Mio. Iterationen auf Knoten 0x00408818).

Dieser Patch ersetzt das "bne" durch ein NOP, damit in jedem Durchlauf
ausgekettet und der Zaehler dekrementiert wird.

Idempotent; aendert nur out/exefs/code_decomp.bin.
Danach `python tools/build_port.py` laufen lassen.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
ADDR = 0x00274CB4
BASE = 0x100000
NOP = 0xE1A00000

with open(PATH, 'rb') as f:
    data = bytearray(f.read())

off = ADDR - BASE
old = struct.unpack_from('<I', data, off)[0]
if old == NOP:
    print('bereits gepatcht: 0x%X ist NOP' % ADDR)
else:
    print('patche 0x%X: %08X -> %08X' % (ADDR, old, NOP))
    struct.pack_into('<I', data, off, NOP)
    with open(PATH, 'wb') as f:
        f.write(data)
