import struct

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000


def u32(a):
    return struct.unpack_from('<I', d, a - base)[0]


for a in (0x455D2C, 0x455D30, 0x455D34, 0x455D38, 0x455D3C, 0x455D40):
    print(hex(a), '%08X' % u32(a))

print('0x455D00:', d[0x455D00 - base:0x455D30 - base].hex())
print('0x455E20:', d[0x455E20 - base:0x455E40 - base].hex())

# Wo zeigt 0x455D30 hin?
p = u32(0x455D30)
print('ptr low =', hex(p))
