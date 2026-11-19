"""Diagnose-Patch: RomFS-Mount nicht frueh aussteigen lassen.

fn_00284D24 prueft nach fn_0011B98C:

    0x284D64  cmp  r0, #0
    0x284D68  beq  0x285188      ; Mount gilt als fehlgeschlagen

Diese Diagnose ersetzt das beq durch ein NOP, damit der Mount-Pfad
weiterlaeuft und fn_00284D24 mit 1 zurueckkehrt (belegt per HLE_ROMFS_LOG:
0x284D24 -> 0x284D80 -> 0x284FD8 -> 0x285184 r0=1).

Das Skript ist idempotent und aendert nur out/exefs/code_decomp.bin.
Danach `python tools/build_port.py` laufen lassen.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
ADDR = 0x00284D68
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
