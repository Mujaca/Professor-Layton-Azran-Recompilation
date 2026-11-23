"""Q4 (schnell): Atlanten >=400x240 als Unterbereich des 400x240-Screens.

Grobscan auf halber Aufloesung (Schritt 8), Feinscan (±8 px, Schritt 1).
"""
import sys
import time

import numpy as np

sys.path.insert(0, 'tools')
import _atest_botscan as BS
import _atest_imgc_np as D

REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)
RW, RH = 400, 240


def half(a):
    h, w = a.shape[0] // 2 * 2, a.shape[1] // 2 * 2
    a = a[:h, :w].astype(np.float64)
    return a.reshape(h // 2, 2, w // 2, 2, -1).mean(axis=(1, 3))


REFH = half(REF)
RWH, RHH = 200, 120


def coarse(img):
    h, w = img.shape[:2]
    a = half(img)
    ah, aw = a.shape[:2]
    if ah < RHH or aw < RWH:
        return []
    out = []
    for y0 in range(0, ah - RHH + 1, 8):
        for x0 in range(0, aw - RWH + 1, 8):
            d = np.abs(a[y0:y0 + RHH, x0:x0 + RWH] - REFH).sum(2)
            out.append((int((d > 60).sum()), x0 * 2, y0 * 2))
    out.sort()
    return out[:3]


def fine(img, x0, y0):
    h, w = img.shape[:2]
    a = img.astype(np.int16)
    b = REF.astype(np.int16)
    best = None
    for dy in range(-8, 9):
        yy = y0 + dy
        if yy < 0 or yy + RH > h:
            continue
        for dx in range(-8, 9):
            xx = x0 + dx
            if xx < 0 or xx + RW > w:
                continue
            d = np.abs(a[yy:yy + RH, xx:xx + RW] - b).sum(2)
            dd = int((d > 60).sum())
            if best is None or dd < best[0]:
                best = (dd, xx, yy)
    return best


def main():
    rows = []
    t0 = time.time()
    for fa in BS.FAS:
        d = open(fa, 'rb').read()
        i = 0
        while True:
            o = d.find(b'IMGC00', i)
            if o < 0:
                break
            i = o + 1
            hd = D.parse_header(d, o)
            if not hd or hd['bits'] not in (24, 32):
                continue
            if hd['w'] < RW or hd['h'] < RH:
                continue
            img = BS.decode_linear(d, o)
            if img is None:
                rr = D.decode_buf(d, o)
                if rr is None:
                    continue
                img = rr[2]
            cand = coarse(img)
            if not cand:
                continue
            best = None
            for (_, x0, y0) in cand:
                r = fine(img, x0, y0)
                if r and (best is None or r[0] < best[0]):
                    best = r
            if best:
                rows.append((best[0], best[1], best[2], fa, o, hd['bits'],
                             hd['w'], hd['h']))
    rows.sort()
    print('kandidaten', len(rows), 'dauer %.1fs' % (time.time() - t0))
    for r in rows[:14]:
        print('%6d/%-6d off=%d,%d  %-14s off=%-10d bits=%2d %4dx%-4d'
              % (r[0], RW * RH, r[1], r[2], r[3].split('/')[-1], r[4],
                 r[5], r[6], r[7]))


if __name__ == '__main__':
    main()
