"""Scan: bestes NCC aller IMGC-Atlanten gegen die Titel-Referenz."""
import struct
import sys
import time

import numpy as np

sys.path.insert(0, 'tools')
import _atest_imgc_np as D

FAS = ['out/romfs/lt6/arc/lt6_b.fa',
       'out/romfs/lt6/arc/lt6_uk.fa',
       'out/romfs/lt6/arc/lt6_a.fa']
PIX = open('reports/menu_official.bin', 'rb').read()
REF = np.frombuffer(PIX, dtype=np.uint8).reshape(240, 400, 3)[:226]
L = REF.astype(np.float64).mean(axis=2)
LH, LW = L.shape
L0 = L - L.mean()
L2 = float((L0 ** 2).sum())
N = LH * LW
fh = 1024
fw = 1024
FL0 = np.conj(np.fft.rfft2(L0, s=(fh, fw)))


def ncc_max(img):
    t = img.astype(np.float64)
    th, tw = t.shape
    sy, sx = th - LH + 1, tw - LW + 1
    if sy <= 0 or sx <= 0:
        return None
    ii = np.zeros((th + 1, tw + 1))
    ii[1:, 1:] = np.cumsum(np.cumsum(t, 0), 1)
    ii2 = np.zeros((th + 1, tw + 1))
    ii2[1:, 1:] = np.cumsum(np.cumsum(t * t, 0), 1)
    S1 = (ii[LH:LH + sy, LW:LW + sx] - ii[0:sy, LW:LW + sx]
          - ii[LH:LH + sy, 0:sx] + ii[0:sy, 0:sx])
    S2 = (ii2[LH:LH + sy, LW:LW + sx] - ii2[0:sy, LW:LW + sx]
          - ii2[LH:LH + sy, 0:sx] + ii2[0:sy, 0:sx])
    F = np.fft.rfft2(t, s=(fh, fw)) * FL0
    cc = np.fft.irfft2(F, s=(fh, fw))[:sy, :sx]
    mean = S1 / N
    var = np.maximum(S2 - N * mean * mean, 1e-6)
    ncc = cc / (np.sqrt(var) * np.sqrt(L2))
    k = np.unravel_index(np.argmax(ncc), ncc.shape)
    return float(ncc[k]), int(k[1]), int(k[0]), th, tw


def main():
    rows = []
    best = {}
    t0 = time.time()
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
            bits = hd['bits']
            if bits not in (4, 8, 24, 32):
                continue
            if bits in (4, 8) and (hd['w'] < 400 or hd['h'] < 226):
                continue
            try:
                r = D.decode_buf(d, o)
            except Exception:
                continue
            if r is None:
                continue
            w, h, rgb, al = r
            res = ncc_max(rgb.astype(np.float64).mean(axis=2))
            if res is None:
                continue
            rows.append((res[0], fa, o, bits, w, h, res[1], res[2]))
    rows.sort(key=lambda z: -z[0])
    print('atlanten', len(rows), 'dauer %.1fs' % (time.time() - t0))
    for r in rows[:25]:
        print('NCC %.4f  %-14s off=%-10d bits=%2d %4dx%-4d at x=%d y=%d'
              % (r[0], r[1].split('/')[-1], r[2], r[3], r[4], r[5], r[6],
                 r[7]))


if __name__ == '__main__':
    main()
