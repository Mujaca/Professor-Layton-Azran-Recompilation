import struct
d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000


def w(a):
    return struct.unpack_from('<I', d, a - base)[0]


for a in range(0x2758E0, 0x275950, 4):
    print('%08X %08X' % (a, w(a)))
