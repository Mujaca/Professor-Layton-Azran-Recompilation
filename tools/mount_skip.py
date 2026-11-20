"""Schaltet den Mount-Skip (tools/patch_mount_skip.py) ein oder aus.

  python tools/mount_skip.py off   -> echter Lader fn_0011E494
  python tools/mount_skip.py on    -> Lader ueberspringen (Diagnose)
"""

import struct
import sys

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000
PATCH = [
    (0x002850A8, 0xE3A08001, 0xEBFA64F9),   # mov r8,#1   |  bl 0x11E494
    (0x002850AC, 0xE1A00000, 0xE1B08000),   # nop         |  movs r8,r0
]

mode = (sys.argv[1] if len(sys.argv) > 1 else 'off').lower()
d = bytearray(open(PATH, 'rb').read())
for addr, on_val, off_val in PATCH:
    val = on_val if mode == 'on' else off_val
    struct.pack_into('<I', d, addr - BASE, val)
    print('%08X <- %08X' % (addr, val))
open(PATH, 'wb').write(d)
print('mount-skip:', mode)
