import struct

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
n = len(d) // 4
TARGET = 0x2144

for i in range(n):
    a = base + i * 4
    ins = struct.unpack_from('<I', d, i * 4)[0]
    # STR/STRB/STRH immediate, P=1 U=1 W=0 L=0
    if (ins & 0x0C000000) != 0x04000000:
        continue
    P = (ins >> 24) & 1
    U = (ins >> 23) & 1
    W = (ins >> 21) & 1
    L = (ins >> 20) & 1
    if L or not P or not U or W:
        continue
    rot = ((ins >> 8) & 0xF) * 2
    v = ins & 0xFF
    val = ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF if rot else v
    if val == TARGET:
        kind = 'STRH' if (ins & 0x60) == 0x40 else 'STRB' if (ins & 0x60) == 0x20 else 'STR'
        print('%08X %s %08X' % (a, kind, ins))
