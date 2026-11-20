import struct

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
n = len(d) // 4
for imm in (0x178, 0x17C):
    for name, op in (('LDR', 0xE5900000), ('STR', 0xE5800000)):
        hits = []
        for i in range(n):
            a = base + i * 4
            ins = struct.unpack_from('<I', d, i * 4)[0]
            if (ins & 0xFFF00000) == op and (ins & 0xFFF) == imm:
                hits.append(a)
        print('%s [Rn,#0x%X]: %d %s' % (name, imm, len(hits), [hex(x) for x in hits[:24]]))
