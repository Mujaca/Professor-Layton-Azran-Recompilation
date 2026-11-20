import struct

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
for a in range(0x12493C, 0x124990, 4):
    print('%08X %08X' % (a, struct.unpack_from('<I', d, a - base)[0]))
