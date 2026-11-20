import struct

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
n = len(d) // 4
target = 0x003D91D0
for i in range(n):
    a = base + i * 4
    v = struct.unpack_from('<I', d, i * 4)[0]
    if v == target:
        for j in range(n):
            b = base + j * 4
            ins = struct.unpack_from('<I', d, j * 4)[0]
            if (ins & 0x0F7F0000) == 0x051F0000:
                lit = b + 8 + (ins & 0xFFF)
                if lit == a:
                    print('used by %08X' % b)
