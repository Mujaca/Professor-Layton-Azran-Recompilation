import struct

p = 'out/exefs/code_decomp.bin'
d = bytearray(open(p, 'rb').read())
o = 0x2927B8 - 0x100000
# Thumb-Stub: ldr r0,[pc,#0] ; bx lr ; .word 0x00455D40
struct.pack_into('<H', d, o, 0x4800)
struct.pack_into('<H', d, o + 2, 0x4770)
struct.pack_into('<I', d, o + 4, 0x00455D40)
open(p, 'wb').write(d)
d2 = open(p, 'rb').read()
print('%04X %04X %08X' % (struct.unpack_from('<H', d2, o)[0],
                          struct.unpack_from('<H', d2, o + 2)[0],
                          struct.unpack_from('<I', d2, o + 4)[0]))
