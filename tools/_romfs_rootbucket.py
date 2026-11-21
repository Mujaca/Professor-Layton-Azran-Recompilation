"""Wurzelverzeichnis in den Dir-Hash-Bucket 0 haengen.

hash("", seed=0) = 0x75BCD15 -> Bucket 0. Der Bucket zeigte aber auf 0x18
('lt6'), nicht auf den Root-Record 0. Dadurch wird der leere erste
Pfadbestandteil falsch aufgeloest (current dir != 0) und die .fa-Pfade
scheitern. Fix: Bucket 0 -> Root-Record 0, Root.hashnext -> alter Kopf.
"""
import struct

PATH = 'out/romfs_image.bin'
DH = 0x1000 + 0x28
DM = 0x1000 + 0x4C

d = bytearray(open(PATH, 'rb').read())
old = struct.unpack_from('<I', d, DH)[0]
root_name_len = struct.unpack_from('<I', d, DM + 0x14)[0]
print('bucket0 old=%04X root namelen=%d' % (old, root_name_len))
struct.pack_into('<I', d, DH, 0)
struct.pack_into('<I', d, DM + 0x10, old)
open(PATH, 'wb').write(d)
print('bucket0 -> 0000, root.hashnext -> %04X' % old)
