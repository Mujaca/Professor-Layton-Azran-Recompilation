"""Robuster RomFS-Fallback: alle File-Buckets -> vollstaendige Record-Kette.

Die Engine-Lookup hasht pro Pfadkomponente und laeuft dann HASHNEXT ab. Im Port
stimmt der Hash-/Seed-Pfad fuer .fa nicht, obwohl die Records erreichbar sind.
Hier zeigen alle 613 Buckets auf eine einzige, vollstaendige Kette aller
File-Records; der Namens-/Parent-Vergleich waehlt weiterhin den richtigen
Record. Nur fuer die Diagnose/den Port, nicht fuer die "pristine" Referenz.
"""
import struct

PATH = 'out/romfs_image.bin'
BLOB = 0x1000
FILE_META = BLOB + 0xAF4
FILE_HASH = BLOB + 0x160
FILE_BUCKETS = 0x994 // 4
FILE_META_SIZE = 0xD284

d = bytearray(open(PATH, 'rb').read())

recs = []
off = 0
while off < FILE_META_SIZE:
    base = FILE_META + off
    nlen = struct.unpack_from('<I', d, base + 0x1C)[0]
    recs.append(off)
    off += 0x20 + ((nlen + 3) & ~3)
    if nlen == 0 or nlen > 0x400:
        break
print('records', len(recs), 'last-off', hex(recs[-1]))

# full chain
for i, rec in enumerate(recs):
    nxt = recs[i + 1] if i + 1 < len(recs) else 0xFFFFFFFF
    struct.pack_into('<I', d, FILE_META + rec + 0x18, nxt)

# all buckets -> first record
for b in range(FILE_BUCKETS):
    struct.pack_into('<I', d, FILE_HASH + b * 4, recs[0])

open(PATH, 'wb').write(d)
print('alle %d Buckets -> %04X, Kette %d Records' % (FILE_BUCKETS, recs[0], len(recs)))
