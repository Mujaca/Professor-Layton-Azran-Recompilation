import struct

p = 'out/exefs/code_decomp.bin'
d = bytearray(open(p, 'rb').read())
struct.pack_into('<H', d, 0x292650 - 0x100000, 0x2800)
struct.pack_into('<H', d, 0x292652 - 0x100000, 0xB510)
open(p, 'wb').write(d)
d2 = open(p, 'rb').read()
print('292650:', hex(struct.unpack_from('<H', d2, 0x292650 - 0x100000)[0]),
      hex(struct.unpack_from('<H', d2, 0x292652 - 0x100000)[0]))
