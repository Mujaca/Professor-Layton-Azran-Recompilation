import struct

p = 'out/exefs/code_decomp.bin'
d = bytearray(open(p, 'rb').read())
off = 0x1009E4 - 0x100000
struct.pack_into('<H', d, off, 0xF7FF)
struct.pack_into('<H', d, off + 2, 0xF842)
open(p, 'wb').write(d)
d2 = open(p, 'rb').read()
print('1009E4:', hex(struct.unpack_from('<H', d2, off)[0]),
      hex(struct.unpack_from('<H', d2, off + 2)[0]))
