"""Untersucht info_a.xi Dekomposition: Tabelle und Tile-Inhalt."""
import struct
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _imgc
import _atest_imgc_np as D

F = 'out/romfs/lt6/arc/lt6_uk.fa'
O = 0x75CCC4


def main():
    d = open(F, 'rb').read()
    o = O
    s1, s2, sz = struct.unpack_from('<iii', d, o + 0x34)
    tab = np.frombuffer(_imgc.decomp(d[o + 0x48:o + 0x48 + s1]),
                        dtype='<u2')
    tex = _imgc.decomp(d[o + 0x48 + s2:o + 0x48 + s2 + sz])
    print('tab entries', len(tab), 'min', tab.min(), 'max', tab.max(),
          'nonzero', int((tab > 0).sum()), 'uniq', len(np.unique(tab)))
    print('first 40', tab[:40].tolist())
    print('tex len', len(tex), 'tiles', len(tex) // 192)
    n = len(tex) // 192
    t = np.frombuffer(tex, dtype=np.uint8)[:n * 192].reshape(n, 8, 8, 3)
    print('tile0 bgr row0', [tuple(int(v) for v in px) for px in t[0, 0]])
    print('tile1 bgr row0', [tuple(int(v) for v in px) for px in t[1, 0]])
    # wie viele Tiles sind komplett schwarz?
    print('black tiles', int((t.sum(axis=(1, 2, 3)) == 0).sum()), 'of', n)
    # vorkommende Farben
    u, c = np.unique(t.reshape(-1, 3), axis=0, return_counts=True)
    print('uniq colors', len(u), 'top', sorted(zip(c.tolist(),
          [tuple(int(x) for x in y) for y in u]), reverse=True)[:5])
    r = D.decode_buf(d, o)
    w, h, rgb, al = r
    print('decoded mean', rgb.mean(axis=(0, 1)), 'uniq',
          len(np.unique(rgb.reshape(-1, 3), axis=0)))


if __name__ == '__main__':
    main()
