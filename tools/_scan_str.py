import struct

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
n = len(d) // 4


def check(target):
    hits = []
    for i in range(n):
        a = base + i * 4
        ins = struct.unpack_from('<I', d, i * 4)[0]
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
        if val == target:
            hits.append((a, ins))
    return hits


for t in (0x1B8, 0x2144, 0x144, 0x2000):
    print('== target', hex(t))
    for a, ins in check(t):
        print('  %08X %08X' % (a, ins))
