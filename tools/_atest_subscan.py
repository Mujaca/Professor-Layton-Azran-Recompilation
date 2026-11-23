"""Q4: Scan aller Atlanten >=400x240 als Unterbereich des Screens."""
import sys
import time

import numpy as np

sys.path.insert(0, 'tools')
import _atest_botscan as BS
import _atest_imgc_np as D

REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)
RW, RH = 400, 240
STEP = 2


def scan(img, name, fa, o, bits):
    h, w = img.shape[:2]
    if h < RH or w < RW:
        return None
    a = img.astype(np.int16)
    b = REF.astype(np.int16)
    best = None
    for y0 in range(0, h - RH + 1, STEP):
        for x0 in range(0, w - RW + 1, STEP):
            d = np.abs(a[y0:y0 + RH, x0:x0 + RW] - b).sum(2)
            dd = int((d > 60).sum())
            if best is None or dd < best[0]:
                best = (dd, x0, y0)
    return (best[0], best[1], best[2], name, fa, o, bits, w, h)


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
            if hd['bits'] == 24:
                img = BS.decode_linear(d, o)
                if img is not None:
                    r = scan(img, 'lin', fa, o, 24)
                    if r:
                        rows.append(r)
            rr = D.decode_buf(d, o)
            if rr is not None:
                r = scan(rr[2], 'swiz', fa, o, hd['bits'])
                if r:
                    rows.append(r)
    rows.sort()
    print('kandidaten', len(rows), 'dauer %.1fs' % (time.time() - t0))
    for r in rows[:14]:
        print('%6d/%-6d off=%d,%d  %-5s %-14s off=%-10d bits=%2d %4dx%-4d'
              % (r[0], RW * RH, r[1], r[2], r[3], r[4].split('/')[-1], r[5],
                 r[6], r[7], r[8]))


if __name__ == '__main__':
    main()
