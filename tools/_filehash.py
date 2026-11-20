import struct
import zlib

img = open('out/romfs_image.bin', 'rb').read(0x1000 + 0xDD80)
base = 0x1000
fh_off = 0x160
fh_size = 0x994
fm_off = 0xAF4

fh = img[base + fh_off: base + fh_off + fh_size]
n = len(fh) // 4
entries = [struct.unpack_from('<I', fh, i * 4)[0] for i in range(n)]
print('File-Hash-Tabelle: %d Eintraege, erste: %s' % (n, [hex(x) for x in entries[:4]]))

names = []
for line in open('reports/romfs_listing.tsv', encoding='utf-8'):
    parts = line.rstrip('\n').split('\t')
    if len(parts) >= 2 and parts[0].startswith('/'):
        names.append(parts[0])

eset = set(entries)
hit = 0
samples = []
for p in names:
    cands = [p, p.lstrip('/'), '/' + p.lstrip('/'), p.split('/')[-1]]
    for c in cands:
        h = zlib.crc32(c.encode()) & 0xFFFFFFFF
        if h in eset or ((~h) & 0xFFFFFFFF) in eset:
            hit += 1
            if len(samples) < 5:
                samples.append((p, c, hex(h)))
            break
print('Treffer mit CRC32:', hit, 'von', len(names))
print('Beispiele:', samples)
