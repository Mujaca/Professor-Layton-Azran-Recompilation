"""Vollstaendige Kette fuer die RomFS-Dir-Hash-Tabelle.

Wie _romfs_fullchain.py, aber fuer Verzeichnisse:
  DIR_META = 0x1000+0x4C, DIR_HASH = 0x1000+0x28, 9 Buckets.
Alle Buckets zeigen auf eine Kette aller Dir-Records; der Namensvergleich
waehlt den richtigen Eintrag.
"""
import struct

PATH = 'out/romfs_image.bin'
BLOB = 0x1000
DIR_META = BLOB + 0x4C
DIR_HASH = BLOB + 0x28
DIR_BUCKETS = 0x24 // 4
DIR_META_SIZE = 0x114

d = bytearray(open(PATH, 'rb').read())

recs = []
off = 0
while off < DIR_META_SIZE:
    base = DIR_META + off
    nlen = struct.unpack_from('<I', d, base + 0x14)[0]
    recs.append(off)
    off += 0x18 + ((nlen + 3) & ~3)
    if nlen == 0 and off > 0x18 * 4:
        break
print('dir records', len(recs), [hex(r) for r in recs])

for i, rec in enumerate(recs):
    nxt = recs[i + 1] if i + 1 < len(recs) else 0xFFFFFFFF
    struct.pack_into('<I', d, DIR_META + rec + 0x10, nxt)
for b in range(DIR_BUCKETS):
    struct.pack_into('<I', d, DIR_HASH + b * 4, recs[0])

open(PATH, 'wb').write(d)
print('alle %d Dir-Buckets -> %04X' % (DIR_BUCKETS, recs[0]))
