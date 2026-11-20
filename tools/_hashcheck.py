import struct
import zlib

img = open('out/romfs_image.bin', 'rb').read(0x1000 + 0xDD80)
tbl = img[0x208:0x208 + 0xDF8]
entries = [struct.unpack_from('<I', tbl, i * 4)[0] for i in range(len(tbl) // 4)]
print('Tabelleneintraege:', len(entries))

names = []
for line in open('reports/romfs_listing.tsv', encoding='utf-8'):
    parts = line.rstrip('\n').split('\t')
    if len(parts) >= 2 and parts[0].startswith('/'):
        names.append(parts[0])

print('Namen:', len(names))


def variants(p):
    base = p.lstrip('/')
    return [p, base, '/' + base, base.split('/')[-1]]


targets = {0xCA511805, 0x3EF858AF, 0xC107A750}
found = {}
for p in names:
    for v in variants(p):
        h = zlib.crc32(v.encode()) & 0xFFFFFFFF
        for t in (h, (~h) & 0xFFFFFFFF):
            if t in targets:
                found.setdefault(t, []).append((p, v, 'crc' if t == h else 'not'))

for t, lst in found.items():
    print('%08X ->' % t, lst[:6])

print('fix.xr in Tabelle (crc)?', 0x3EF858AF in entries, ' ~?', 0xC107A750 in entries)
