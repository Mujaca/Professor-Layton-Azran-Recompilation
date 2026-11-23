import struct
import sys

sys.path.insert(0, 'tools')
import _atest_xpck as X
import _imgc

d = open('out/romfs/lt6/arc/lt6_uk.fa', 'rb').read()
for name, off, ln in (('menu/uk/title_a.xa', 0x01428314, 0xFEAA8),
                      ('menu/uk/title_b.xa', 0x01526DC0, 0x9278)):
    cnt, ents = X.parse(d, off, ln, _imgc.decomp)
    print('===', name, 'entries', cnt)
    for e in ents:
        info = ''
        if e['magic'] == 'IMGC00':
            fo = off + e['off']
            w, h = struct.unpack_from('<hh', d, fo + 0x10)
            info = '%dx%d bits=%d' % (w, h, d[fo + 0x0D])
        print('  %-14s off=%08X size=%-8d %-8s %s'
              % (e['name'], e['off'], e['size'],
                 e['magic'].encode('ascii', 'replace').decode('ascii'), info))
