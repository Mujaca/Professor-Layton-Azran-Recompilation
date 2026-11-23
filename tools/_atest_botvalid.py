"""Validierung der Methode: Bottom-Capture gegen title_b_bg + title_b.xa-Sprites."""
import struct
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_imgc_np as D
import _atest_botscan as BS

CAP = np.frombuffer(open('reports/menu_official_bottom.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 320, 3)
UK = open('out/romfs/lt6/arc/lt6_uk.fa', 'rb').read()
TS = 16
STEP = 4


def patches(img, step):
    h, w = img.shape[:2]
    ys = np.arange(0, h - TS + 1, step)
    xs = np.arange(0, w - TS + 1, step)
    yy = ys[:, None] + np.arange(TS)[None, :]
    xx = xs[:, None] + np.arange(TS)[None, :]
    a = img[yy[:, None, :, None], xx[None, :, None, :]].reshape(-1, TS, TS, 3)
    gy, gx = np.meshgrid(ys, xs, indexing='ij')
    return a.astype(np.int16), gx.ravel(), gy.ravel()


def main():
    tiles = [(x, y) for y in range(0, 240, TS) for x in range(0, 320, TS)]
    rt = np.stack([CAP[y:y + TS, x:x + TS] for (x, y) in tiles]).astype(np.int16)
    res = {}
    for name, off in (('title_b_bg', 0xCE43074),):
        img = BS.decode_linear(open(
            'out/romfs/lt6/arc/lt6_b.fa', 'rb').read(), off)
        res[name] = img
    # title_b.xa IMGC
    cnt, ents = None, None
    sys.path.insert(0, 'tools')
    import _atest_xpck as X
    import _imgc
    cnt, ents = X.parse(UK, 0x01526DC0, 0x9278, _imgc.decomp)
    for e in ents:
        if e['magic'] == 'IMGC00':
            o = 0x01526DC0 + e['off']
            rr = D.decode_buf(UK, o)
            print('title_b.xa %s %dx%d bits=%d' % (e['name'], rr[0], rr[1],
                                                   UK[o + 0x0D]))
            res['title_b.xa:' + e['name']] = rr[2]
    best = np.full(len(tiles), 9999.0)
    src = [''] * len(tiles)
    for name, img in res.items():
        a, gx, gy = patches(img, STEP)
        flat = a.reshape(len(a), -1).astype(np.int32)
        for j, (x, y) in enumerate(tiles):
            t = rt[j].reshape(-1).astype(np.int32)
            sad = np.abs(flat - t).mean(axis=1)
            k = int(np.argmin(sad))
            if sad[k] < best[j]:
                best[j] = float(sad[k])
                src[j] = '%s u=%d v=%d' % (name, gx[k], gy[k])
    for thr in (4, 6, 8, 12):
        print('Bottom: Kacheln mit SAD<=%2d: %d/%d (%.1f%%)'
              % (thr, int((best <= thr).sum()), len(tiles),
                 (best <= thr).mean() * 100))
    print('median SAD %.2f' % np.median(best))
    for j in np.argsort(best)[:6]:
        print('  best %5.1f  Kachel %s  %s' % (best[j], tiles[j], src[j]))


if __name__ == '__main__':
    main()
