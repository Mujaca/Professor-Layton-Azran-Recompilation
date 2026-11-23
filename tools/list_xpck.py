import os
import struct
import sys

os.chdir(r'C:\Users\user\Documents\Repos\3DS')
sys.path.insert(0, r'C:\Users\user\Documents\Repos\3DS\tools')
import _imgc

d = open('out/fa_uk/menu/uk/title_b.xa', 'rb').read()
fc1, fc2 = d[4], d[5]
count = ((fc2 & 0xF) << 8) | fc1
info_off = struct.unpack_from('<H', d, 6)[0] << 2
name_off = struct.unpack_from('<H', d, 8)[0] << 2
data_off = struct.unpack_from('<H', d, 0xA)[0] << 2
info_size = struct.unpack_from('<H', d, 0xC)[0] << 2
name_size = struct.unpack_from('<H', d, 0xE)[0] << 2
data_size = struct.unpack_from('<I', d, 0x10)[0] << 2
print('count', count, 'info', hex(info_off), hex(info_size),
      'names', hex(name_off), hex(name_size),
      'data', hex(data_off), hex(data_size), 'file', hex(len(d)))
names = _imgc.decomp(d[name_off:name_off + name_size])
print('name table', len(names))
for i in range(count):
    e = info_off + i * 12
    h, no, t1, t2, z1, z2 = struct.unpack_from('<IHHHBB', d, e)
    foff = ((z1 << 16) | t1) << 2
    fsize = (z2 << 16) | t2
    s = names[no:names.find(b'\0', no)].decode('latin1') if no < len(names) else '?'
    print('%2d %08X %-20s off=%08X size=%d' % (i, h, s, data_off + foff, fsize))
