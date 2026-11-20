import struct

d = open('out/romfs_image.bin', 'rb').read(0x20000)
for off in (0x12C8, 0x22C8, 0x117C, 0x217C, 0x1A74, 0x2A74, 0x1504, 0x2504, 0x1640, 0x2640):
    print('%06X %08X' % (off, struct.unpack_from('<I', d, off)[0]))
