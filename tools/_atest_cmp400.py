"""Vergleicht die besten 400x240-IMGCs mit der Referenz und prueft
Kanal-/Gamma-Hypothesen."""
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_imgc_np as D

REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)[:226].astype(np.float64)


def linfit(x, y):
    xm = x.mean()
    ym = y.mean()
    vx = ((x - xm) ** 2).sum()
    if vx == 0:
        return 0.0, ym, 0.0
    a = ((x - xm) * (y - ym)).sum() / vx
    b = ym - a * xm
    c = float(np.corrcoef(x, y)[0, 1])
    return a, b, c


def main():
    res = []
    for fa in ('out/romfs/lt6/arc/lt6_b.fa', 'out/romfs/lt6/arc/lt6_uk.fa'):
        d = open(fa, 'rb').read()
        i = 0
        while True:
            o = d.find(b'IMGC00', i)
            if o < 0:
                break
            i = o + 1
            hd = D.parse_header(d, o)
            if not hd or (hd['w'], hd['h']) != (400, 240):
                continue
            r = D.decode_buf(d, o)
            if r is None:
                continue
            w, h, rgb, al = r
            a = rgb[:226].astype(np.float64)
            dm = np.abs(a - REF).sum(2)
            res.append((int((dm > 60).sum()), float(np.abs(a - REF).mean()),
                        fa, o, hd['bits'], rgb))
    res.sort(key=lambda z: z[0])
    print('--- beste 400x240-Kandidaten ---')
    for x in res[:6]:
        print('diff %6d mean %6.2f %-14s off=%d bits=%d' %
              (x[0], x[1], x[2].split('/')[-1], x[3], x[4]))
    dd, mn, fa, o, bits, rgb = res[0]
    a = rgb[:226].astype(np.float64)
    print('--- Kanal-Fit (bester Kandidat) ---')
    for c, name in enumerate('RGB'):
        sl = linfit(a[:, :, c].ravel(), REF[:, :, c].ravel())
        print('%s: a=%.3f b=%.2f corr=%.4f  mean_cand=%.1f mean_ref=%.1f' %
              (name, sl[0], sl[1], sl[2], a[:, :, c].mean(),
               REF[:, :, c].mean()))


if __name__ == '__main__':
    main()
