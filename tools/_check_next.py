import struct

d = open('out/romfs_image.bin', 'rb').read()
for off in (0x3440, 0x2AF8, 0x2CD0, 0x347C, 0x34C8):
    print('%06X %08X' % (off, struct.unpack_from('<I', d, off)[0]))
