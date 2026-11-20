"""Diagnose-Patch: memcpy-Beschleunigung.

fn_00291B60 ist die 32-Byte-Kopierschleife der Engine (push {..}, ldm/stm,
pop). Im interpretierten Port kostet eine 3-MB-Kopie Minuten. Dieser Patch
ersetzt die erste Instruktion durch `bx lr`; der Trace-Hook (HLE_FAST_MEMCPY,
Default AN) erledigt die Kopie dann nativ mit memmove().

Idempotent; aendert nur out/exefs/code_decomp.bin.
Danach `python tools/build_port.py` laufen lassen.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
ADDR = 0x00291B60
BASE = 0x100000
BX_LR = 0xE12FFF1E

with open(PATH, 'rb') as f:
    data = bytearray(f.read())

off = ADDR - BASE
old = struct.unpack_from('<I', data, off)[0]
if old == BX_LR:
    print('bereits gepatcht: 0x%X ist bx lr' % ADDR)
else:
    print('patche 0x%X: %08X -> %08X' % (ADDR, old, BX_LR))
    struct.pack_into('<I', data, off, BX_LR)
    with open(PATH, 'wb') as f:
        f.write(data)
