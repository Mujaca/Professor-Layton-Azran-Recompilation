"""NCC per FFT: enthaelt eine Textur die Referenz als Unterbereich?"""
import struct
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_titlexc as T
import _atest_imgc_np as D

REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)[:226]
L = REF.astype(np.float64).mean(axis=2)
LH, LW = L.shape
L0 = L - L.mean()
L2 = float((L0 ** 2).sum())


def ncc_map(img):
    """NCC der Referenz gegen jedes Fenster gleicher Groesse in img."""
    t = img.astype(np.float64)
    th, tw = t.shape
    # Fenster 226x400 in t
    n = LH * LW
    # Integralbilder fuer Summe und Quadratsumme
    ii = np.zeros((th + 1, tw + 1))
    ii[1:, 1:] = np.cumsum(np.cumsum(t, 0), 1)
    ii2 = np.zeros((th + 1, tw + 1))
    ii2[1:, 1:] = np.cumsum(np.cumsum(t * t, 0), 1)
    sy, sx = th - LH + 1, tw - LW + 1
    if sy <= 0 or sx <= 0:
        return None
    S1 = (ii[LH:LH + sy, LW:LW + sx] - ii[0:sy, LW:LW + sx]
          - ii[LH:LH + sy, 0:sx] + ii[0:sy, 0:sx])
    S2 = (ii2[LH:LH + sy, LW:LW + sx] - ii2[0:sy, LW:LW + sx]
          - ii2[LH:LH + sy, 0:sx] + ii2[0:sy, 0:sx])
    # Kreuzkorrelation sum(t*L) fuer alle Offsets via FFT
    fh = 1 << int(np.ceil(np.log2(th + LH - 1)))
    fw = 1 << int(np.ceil(np.log2(tw + LW - 1)))
    F = np.fft.rfft2(t, s=(fh, fw)) * np.conj(np.fft.rfft2(L0, s=(fh, fw)))
    cc = np.fft.irfft2(F, s=(fh, fw))
    # cc[dy,dx] = sum t[dy:dy+LH, dx:dx+LW] * L0  (Korrelation)
    cc = cc[:sy, :sx]
    mean = S1 / n
    var = S2 - n * mean * mean
    var = np.maximum(var, 0)
    ncc = cc / (np.sqrt(var) * np.sqrt(L2) + 1e-9)
    return ncc


def main():
    entries = [e for e in T.ents if e[5] == b'IMGC00']
    print('texturen', len(entries))
    for (i, h, name, off, size, mg, info) in entries:
        r = D.decode_buf(T.d, off)
        if r is None:
            print('%-10s decode fail' % name)
            continue
        w, hh, rgb, al = r
        img = rgb.astype(np.float64).mean(axis=2)
        nm = ncc_map(img)
        if nm is None:
            print('%-10s %4dx%-4d zu klein' % (name, w, hh))
            continue
        k = np.unravel_index(np.argmax(nm), nm.shape)
        print('%-10s %4dx%-4d best NCC %.4f at x=%d y=%d'
              % (name, w, hh, nm[k], k[1], k[0]))
        np.save('tools/out/atest_ncc_%s.npy' % name.replace('.', '_'), nm)


if __name__ == '__main__':
    main()
