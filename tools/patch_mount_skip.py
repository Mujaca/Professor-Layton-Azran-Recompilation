"""Diagnose-Patch: Archiv-Dateilader im Mount ueberspringen.

fn_00284D24 ruft bei 0x2850A8 fn_0011E494 (Archiv-Datei laden). Dieser Pfad
haengt im Port (fn_0012493C -> 0x130220 -> 0x13A080 -> 0x287DA4 ->
fn_002926DC -> 0x292650 -> fn_001009E4, weil dort ein Stack-Zeiger als
Heap-Zeiger freigegeben wird).

Der Patch ersetzt:

    0x2850A8  bl 0x11E494        ->  mov r8, #1
    0x2850AC  movs r8, r0        ->  nop

Damit gilt der Ladevorgang als erfolgreich (r8 = 1) und fn_00284D24 laeuft
ueber 0x2850B8 bis 0x285184 weiter. Belegt per HLE_ROMFS_LOG:
[romfs] 002850B8 ... und [romfs] 00285184 r0=0.

Idempotent; aendert nur out/exefs/code_decomp.bin.
Danach `python tools/build_port.py` laufen lassen.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000
PATCHES = [
    (0x002850A8, 0xE3A08001),   # mov r8, #1
    (0x002850AC, 0xE1A00000),   # nop
]

with open(PATH, 'rb') as f:
    data = bytearray(f.read())

for addr, val in PATCHES:
    off = addr - BASE
    old = struct.unpack_from('<I', data, off)[0]
    if old == val:
        print('bereits gepatcht: 0x%X' % addr)
    else:
        print('patche 0x%X: %08X -> %08X' % (addr, old, val))
        struct.pack_into('<I', data, off, val)

with open(PATH, 'wb') as f:
    f.write(data)
