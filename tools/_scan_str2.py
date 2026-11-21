import struct

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
n = len(d) // 4

for target in (0x2144, 0x144):
    print('== STR imm', hex(target))
    for i in range(n):
        a = base + i * 4
        ins = struct.unpack_from('<I', d, i * 4)[0]
        if (ins & 0x0C000000) != 0x04000000:
            continue
        P = (ins >> 24) & 1
        W = (ins >> 21) & 1
        L = (ins >> 20) & 1
        if not P or W or L:
            continue
            off = None
        if (ins & 0x60) == 0x40:      # STRH: imm4H<<4 | imm4L
            off = ((ins >> 4) & 0xF0) | (ins & 0xF)
        elif (ins & 0x60) == 0x20:    # STRB
            off = ins & 0xFFF
        else:                   # STR
            off = ins & 0xFFF
        if off == target:
            print('  %08X %08X' % (a, ins))
