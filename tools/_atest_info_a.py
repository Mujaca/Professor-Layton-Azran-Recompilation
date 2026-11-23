"""Dekodiert menu/uk/info_a.xi (400x240, 24bpp) aus lt6_uk.fa."""
import struct
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _imgc
import _atest_imgc_np as D
F = 'out/romfs/lt6/arc/lt6_uk.fa'
O = 0x75CCC4
REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)


def size_of(b):
    return ((b[0] >> 3) | (b[1] << 5) | (b[2] << 13) | (b[3] << 21))


def main():
    d = open(F, 'rb').read()
    o = O
    s1, s2, sz = struct.unpack_from('<iii', d, o + 0x34)
    tab_raw = d[o + 0x48:o + 0x48 + s1]
    strm = d[o + 0x48 + s2:o + 0x48 + s2 + sz]
    print('table raw', len(tab_raw), 'meth', tab_raw[0] & 7, 'size', size_of(tab_raw))
    print('stream', len(strm), 'meth', strm[0] & 7, 'size', size_of(strm))
    for name, fn in (('_imgc.decomp', _imgc.decomp),):
        try:
            t = fn(bytes(tab_raw))
            print(name, 'table ->', len(t))
            x = fn(bytes(strm))
            print(name, 'stream ->', len(x))
        except Exception as e:
            print(name, 'ERR', e)
    r = D.decode_buf(d, o)
    print('decode_buf ->', None if r is None else (r[0], r[1]))
    if r:
        w, h, rgb, al = r
        dd = np.abs(rgb.astype(np.int16) - REF[:h].astype(np.int16)).sum(2) > 60
        print('diff>60 = %d/%d  mean=%.2f' % (int(dd.sum()), w * h,
                                              float(np.abs(rgb.astype(float)
                                                           - REF[:h]).mean())))
        open('tools/out/atest_info_a.rgb', 'wb').write(rgb.tobytes())


if __name__ == '__main__':
    main()
