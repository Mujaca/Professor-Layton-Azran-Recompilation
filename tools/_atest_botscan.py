"""Bottom-Scan: alle 320x240-IMGCs mit beiden Dekodern gegen den Capture."""
import struct
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _imgc
import _atest_imgc_np as D

FAS = ['out/romfs/lt6/arc/lt6_b.fa',
       'out/romfs/lt6/arc/lt6_uk.fa',
       'out/romfs/lt6/arc/lt6_a.fa']
CAP = np.frombuffer(open('reports/menu_official_bottom.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 320, 3)
VIS = 223


def decode_linear(d, o):
    s1, s2, sz = struct.unpack_from('<iii', d, o + 0x34)
    w, h = struct.unpack_from('<hh', d, o + 0x10)
    bpp = d[o + 0x0D] // 8
    if bpp not in (3, 4):
        return None
    bpt = 64 * bpp
    tab = _imgc.decomp(d[o + 0x48:o + 0x48 + s1])
    tex = _imgc.decomp(d[o + 0x48 + s2:o + 0x48 + s2 + sz])
    if not tab or not tex:
        return None
    if len(tab) >= 2 and struct.unpack_from('<H', tab, 0)[0] == 0x453:
        e = np.frombuffer(tab, dtype='<u4', count=(len(tab) - 8) // 4,
                          offset=8).astype(np.int64)
    else:
        n = len(tab) // 2
        e = np.frombuffer(tab, dtype='<u2', count=n).astype(np.int64)
    ntile = len(tex) // bpt
    src = np.frombuffer(tex, dtype=np.uint8)[:ntile * bpt].reshape(ntile, bpt)
    grid_h, grid_w = (h + 7) // 8, (w + 7) // 8
    n = min(len(e), grid_h * grid_w)
    pic = np.zeros((grid_h * grid_w, bpt), dtype=np.uint8)
    ok = (e[:n] >= 0) & (e[:n] < ntile)
    pic[:n][ok] = src[e[:n][ok]]
    img = pic.reshape(grid_h, grid_w, 8, 8, bpp).transpose(0, 2, 1, 3, 4)
    img = img.reshape(grid_h * 8, grid_w * 8, bpp)[:h, :w]
    if bpp == 3:
        return img[:, :, ::-1].copy()
    return img[:, :, [2, 1, 0]].copy()


def main():
    rows = []
    for fa in FAS:
        d = open(fa, 'rb').read()
        i = 0
        while True:
            o = d.find(b'IMGC00', i)
            if o < 0:
                break
            i = o + 1
            hd = D.parse_header(d, o)
            if not hd or (hd['w'], hd['h']) != (320, 240):
                continue
            if hd['bits'] not in (24, 32):
                continue
            r = decode_linear(d, o)
            if r is not None:
                a = r[:VIS].astype(np.int16)
                dd = int((np.abs(a - CAP[:VIS]).sum(2) > 60).sum())
                rows.append((dd, float(np.abs(a - CAP[:VIS]).mean()),
                             'linear', fa, o))
            rr = D.decode_buf(d, o)
            if rr is not None:
                a = rr[2][:VIS].astype(np.int16)
                dd = int((np.abs(a - CAP[:VIS]).sum(2) > 60).sum())
                rows.append((dd, float(np.abs(a - CAP[:VIS]).mean()),
                             'swiz', fa, o))
    rows.sort()
    print('kandidaten', len(rows))
    for r in rows[:14]:
        print('%6d/%-6d mean %6.2f %-6s %-14s off=%d'
              % (r[0], VIS * 320, r[1], r[2], r[3].split('/')[-1], r[4]))


if __name__ == '__main__':
    main()
