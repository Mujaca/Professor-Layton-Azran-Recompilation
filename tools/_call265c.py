import struct

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
n = len(d) // 4
target = 0x0027656C
for i in range(n):
    a = base + i * 4
    ins = struct.unpack_from('<I', d, i * 4)[0]
    if (ins & 0x0F000000) == 0x0B000000:
        imm = ins & 0xFFFFFF
        if imm & 0x800000:
            imm -= 0x1000000
        t = a + 8 + imm * 4
        if t == target:
            print('bl at %08X' % a)
