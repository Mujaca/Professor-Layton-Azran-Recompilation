import struct

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000

target = 0x1013F0
for a in range(0x101000, 0x101600, 4):
    ins = struct.unpack_from('<I', d, a - base)[0]
    if (ins & 0x0F000000) == 0x0A000000:
        imm = ins & 0xFFFFFF
        if imm & 0x800000:
            imm -= 0x1000000
        t = a + 8 + imm * 4
        if t == target:
            print('B  %08X cond=%X' % (a, (ins >> 28) & 0xF))
    if (ins & 0x0F000000) == 0x0B000000:
        imm = ins & 0xFFFFFF
        if imm & 0x800000:
            imm -= 0x1000000
        t = a + 8 + imm * 4
        if t == target:
            print('BL %08X cond=%X' % (a, (ins >> 28) & 0xF))
