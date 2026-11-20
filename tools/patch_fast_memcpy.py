"""Diagnose-Patch: memcpy-Beschleunigung.

Die Engine hat zwei Kopierschleifen:
  fn_00291B60 - 32-Byte-Kopie (ldm/stm)
  fn_0027D9B4 - memcpy-Einstieg, faellt bei Alignment auf 0x291B60 zurueck,
                sonst byte-weise Kopie (ldrb/strb)

Im interpretierten Port kosten grosse Kopien Minuten. Dieser Patch ersetzt
jeweils die erste Instruktion durch `bx lr`; der Trace-Hook (HLE_FAST_MEMCPY,
Default AN) erledigt die Kopie nativ mit memmove().

Idempotent; aendert nur out/exefs/code_decomp.bin.
Danach `python tools/build_port.py` laufen lassen.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000
BX_LR = 0xE12FFF1E
ADDRS = [0x00291B60, 0x0027D9B4]

with open(PATH, 'rb') as f:
    data = bytearray(f.read())

for addr in ADDRS:
    off = addr - BASE
    old = struct.unpack_from('<I', data, off)[0]
    if old == BX_LR:
        print('bereits gepatcht: 0x%X ist bx lr' % addr)
    else:
        print('patche 0x%X: %08X -> %08X' % (addr, old, BX_LR))
        struct.pack_into('<I', data, off, BX_LR)

with open(PATH, 'wb') as f:
    f.write(data)
