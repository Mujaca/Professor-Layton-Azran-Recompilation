import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _decode_bg

TARGETS = [(400, 240, 'reports/menu_official.bin'),
           (320, 240, 'reports/menu_official_bottom.bin')]


def decode(p):
    w, h, bpp, pic = _decode_bg.decode(p)
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        if bpp >= 3:
            b, g, r = pic[i * bpp], pic[i * bpp + 1], pic[i * bpp + 2]
        else:
            r = g = b = pic[i * bpp]
        rgb[i * 3:i * 3 + 3] = bytes((r, g, b))
    return w, h, bytes(rgb)


def corr(a, b):
    n = min(len(a), len(b)) // 3
    sa = sb = saa = sbb = sab = 0.0
    for i in range(0, n * 3, 3):
        x = (a[i] + a[i + 1] + a[i + 2]) / 3.0
        y = (b[i] + b[i + 1] + b[i + 2]) / 3.0
        sa += x
        sb += y
        saa += x * x
        sbb += y * y
        sab += x * y
    ma = sa / n
    mb = sb / n
    va = saa / n - ma * ma
    vb = sbb / n - mb * mb
    if va <= 0 or vb <= 0:
        return 0.0
    return (sab / n - ma * mb) / ((va ** 0.5) * (vb ** 0.5))


files = []
for pat in ('out/fa_*/**/*.xi', 'out/x_*/*.xi'):
    files += glob.glob(pat, recursive=True)

for w0, h0, refpath in TARGETS:
    ref = open(refpath, 'rb').read()
    rows = []
    for p in files:
        try:
            hdr = open(p, 'rb').read(0x50)
        except Exception:
            continue
        if hdr[:4] != b'IMGC':
            continue
        w, h = struct.unpack_from('<HH', hdr, 0x10)
        if (w, h) != (w0, h0):
            continue
        try:
            dw, dh, rgb = decode(p)
        except Exception:
            continue
        if (dw, dh) != (w0, h0):
            continue
        rows.append((corr(rgb, ref), p))
    rows.sort(key=lambda x: -x[0])
    print('=== %dx%d vs %s : %d Kandidaten' % (w0, h0, refpath, len(rows)))
    for c, p in rows[:8]:
        print('  corr=%.6f  %s' % (c, p))
