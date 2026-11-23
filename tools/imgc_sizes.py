import os
import struct
import sys
import tempfile

sys.path.insert(0, 'tools')
import _imgc
import _atest_imgc_np as D
import title3d_lib as L

FAS = ['out/romfs/lt6/arc/lt6_b.fa', 'out/romfs/lt6/arc/lt6_uk.fa',
       'out/romfs/lt6/arc/lt6_a.fa']
td = tempfile.mkdtemp()
rows = {}
for fa in FAS:
    d = open(fa, 'rb').read()
    i = 0
    while True:
        o = d.find(b'IMGC00', i)
        if o < 0:
            break
        i = o + 1
        hd = D.parse_header(d, o)
        if not hd:
            continue
        s1, s2, sz = hd['s1'], hd['s2'], hd['sz']
        if s1 <= 0 or sz <= 0:
            continue
        stream = d[o + 0x48 + s2:o + 0x48 + s2 + sz]
        method = stream[0] & 7
        dec = L.lz10(stream) if method == 1 else _imgc.decomp(stream)
        key = (method, hd['bits'], struct.unpack_from('<H', d, o + 0x0A)[0])
        ent = rows.setdefault(key, [0, 0, 0, 0])
        ent[0] += 1
        ent[1] += 1 if len(dec) == ((stream[0] >> 3) | (stream[1] << 5) |
                                    (stream[2] << 13) | (stream[3] << 21)) else 0
        ent[2] = max(ent[2], ((stream[0] >> 3) | (stream[1] << 5) |
                              (stream[2] << 13) | (stream[3] << 21)))
        ent[3] = max(ent[3], len(dec))
for k, v in sorted(rows.items()):
    print('method=%d bits=%2d fmt=0x%04X  n=%d lz10_ok=%d maxdecl=%d maxlen=%d'
          % (k[0], k[1], k[2], v[0], v[1], v[2], v[3]))
