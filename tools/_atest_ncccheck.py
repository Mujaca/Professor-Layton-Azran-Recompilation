"""Validierung der FFT-NCC gegen direkte Berechnung."""
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_titlexc as T
import _atest_imgc_np as D

REF = np.frombuffer(open('reports/menu_official.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 400, 3)[:226]
L = REF.astype(np.float64).mean(axis=2)

r = D.decode_buf(T.d, 0x00052E7C)
w, h, rgb, al = r
img = rgb.astype(np.float64).mean(axis=2)
for (dy, dx) in ((285, 2), (0, 0), (100, 100)):
    a = img[dy:dy + 226, dx:dx + 400].ravel()
    b = L.ravel()
    a = a - a.mean()
    b = b - b.mean()
    print(dy, dx, float((a @ b) / np.sqrt((a @ a) * (b @ b))))
