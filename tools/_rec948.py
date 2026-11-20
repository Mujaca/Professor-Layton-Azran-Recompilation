import struct

d = open('out/romfs_image.bin', 'rb').read(0x1000 + 0xDD80)
META = 0x1000  # metadata base in image


def rec(off):
    base = META + off
    parent, nxt = struct.unpack_from('<II', d, base)
    doff, dsz = struct.unpack_from('<QQ', d, base + 8)
    nlen = struct.unpack_from('<I', d, base + 0x18)[0]
    name = d[base + 0x1C: base + 0x1C + nlen].decode('utf-16-le', 'replace')
    return parent, nxt, doff, dsz, name


for off in (0x948, 0x2060):
    print('%04X parent=%08X next=%08X off=%X size=%X name=%r' % ((off,) + rec(off)))

bucket90 = struct.unpack_from('<I', d, META + 0x160 + 90 * 4)[0]
print('bucket90 =', hex(bucket90))
