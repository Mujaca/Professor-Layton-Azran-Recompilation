"""Sucht das 320x240-Asset zum Bottom-Capture (nur die 223 sichtbaren Zeilen)."""
import glob
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc


def bottom_ref():
    a = np.asarray(Image.open(os.path.join(
        ROOT, 'reports', 'azahar_bottomscreen.png')).convert('RGB'))
    return a[:, 56:376].astype(int)


def main():
    ref = bottom_ref()
    rows = ref.shape[0]
    out = []
    for p in glob.glob(os.path.join(ROOT, 'out', 'fa_*', 'menu', '**', '*.xi'),
                       recursive=True):
        try:
            w, h, bpp, data = _imgc.decode(p)
        except Exception:
            continue
        if (w, h) != (320, 240):
            continue
        rgb = np.frombuffer(data, np.uint8).reshape(h, w, bpp)
        if bpp == 3:
            rgb = rgb[:, :, ::-1]
        else:
            rgb = np.repeat(rgb[:, :, :1], 3, axis=2)
        d = np.abs(rgb[:rows].astype(int) - ref).sum(axis=2)
        c = np.abs(rgb[:rows].astype(int) - ref)
        out.append((int((d > 60).sum()), float(c.mean()),
                    os.path.relpath(p, ROOT)))
    out.sort()
    for a, mean, p in out[:20]:
        print('%6d/%6d mean=%.1f  %s' % (a, rows * 320, mean, p))


if __name__ == '__main__':
    main()
