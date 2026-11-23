"""XPCK menu/title.xc (res_map_a) parsen: d, count, ents."""
import struct
import sys

sys.path.insert(0, 'tools')
import _imgc

B = open('out/romfs/lt6/arc/lt6_b.fa', 'rb').read()
OFF = 0x0CD691E0
LEN = 0xD9E90
d = B[OFF:OFF + LEN]

fc1, fc2 = d[4], d[5]
count = ((fc2 & 0xF) << 8) | fc1
info_off = struct.unpack_from('<H', d, 6)[0] << 2
name_off = struct.unpack_from('<H', d, 8)[0] << 2
data_off = struct.unpack_from('<H', d, 0xA)[0] << 2
name_size = struct.unpack_from('<H', d, 0xE)[0] << 2
names = _imgc.decomp(d[name_off:name_off + name_size])
ents = []
for i in range(count):
    e = info_off + i * 12
    h, no, t1, t2, z1, z2 = struct.unpack_from('<IHHHBB', d, e)
    s = names[no:names.find(b'\0', no)].decode('latin1')
    fo = data_off + (((z1 << 16) | t1) << 2)
    ents.append((i, h, s, fo, (z2 << 16) | t2, d[fo:fo + 6], ''))
