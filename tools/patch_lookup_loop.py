"""Diagnose-Patch: Endlos-Schleife im RomFS-Metadaten-Suchlauf beenden.

Die Schleife in fn_00212Cxx:

    0x212DEC  ldr  r4, [sp, #0x118]    ; naechster Eintrag
    0x212DF0  cmn  r4, #1              ; Terminator -1?
    0x212DF4  bne  0x212D3C            ; weiter, solange r4 != -1

Liefert der Metadaten-Eintrag als "next" eine 0 statt -1, laeuft die
Schleife endlos (gemessen: r4 wechselt nie zu -1, >30.000 Iterationen).

Dieser Patch ersetzt `cmn r4,#1` durch `cmp r4,#0`, sodass auch 0 als
Terminator wirkt.

Idempotent; aendert nur out/exefs/code_decomp.bin.
Danach `python tools/build_port.py` laufen lassen.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000
ADDRS = [0x00212DF0, 0x00212F60]
CMP_R4_0 = 0xE3540000

with open(PATH, 'rb') as f:
    data = bytearray(f.read())

for addr in ADDRS:
    off = addr - BASE
    old = struct.unpack_from('<I', data, off)[0]
    if old == CMP_R4_0:
        print('bereits gepatcht: 0x%X ist cmp r4,#0' % addr)
    else:
        print('patche 0x%X: %08X -> %08X' % (addr, old, CMP_R4_0))
        struct.pack_into('<I', data, off, CMP_R4_0)

with open(PATH, 'wb') as f:
    f.write(data)
