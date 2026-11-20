import struct

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
n = len(d) // 4
for imm in (0x1F4, 0x1F5, 0x1F6, 0x1F7):
    hits = []
    for i in range(n):
        a = base + i * 4
        ins = struct.unpack_from('<I', d, i * 4)[0]
        if (ins & 0xFFF00000) == 0xE5C00000 and (ins & 0xFFF) == imm:
            hits.append(a)
    print('STRB [Rn,#0x%X]: %d %s' % (imm, len(hits), [hex(x) for x in hits[:20]]))
