import struct

p = 'out/romfs_image.bin'
d = bytearray(open(p, 'rb').read())
# Bucket-Woerter auf die (bisher unerreichbaren) File-Records umbiegen.
PATCH = [
    (0x12C8, 0x0948),   # product.bcsar
    (0x117C, 0x0000),   # lt6_a.fa
    (0x1A74, 0x01D8),   # m001.moflex
    (0x1504, 0x0984),   # BGM_001
    (0x1640, 0x09D0),   # BGM_002
]
for off, val in PATCH:
    old = struct.unpack_from('<I', d, off)[0]
    struct.pack_into('<I', d, off, val)
    print('%06X: %08X -> %08X' % (off, old, val))
open(p, 'wb').write(d)
print('romfs_image.bin aktualisiert')
