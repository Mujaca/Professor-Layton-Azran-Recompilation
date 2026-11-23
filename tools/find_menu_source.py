"""Sucht das 400x240-Asset, das zum offiziellen Titel-/Menue-Frame passt.

Der Top-Capture (reports/azahar_topscreen.png) ist ein Fensterfoto:
  Screenbereich x=16..415, y=13..238 (226 Zeilen, Rest fehlt).
Verglichen werden daher nur die ersten 226 Zeilen.
"""
import glob
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc


def top_ref():
    a = np.asarray(Image.open(os.path.join(
        ROOT, 'reports', 'azahar_topscreen.png')).convert('RGB'))
    return a[13:239, 16:416].astype(int)


def main():
    ref = top_ref()
    rows = ref.shape[0]
    out = []
    for p in glob.glob(os.path.join(ROOT, 'out', 'fa_*', 'menu', '**', '*.xi'),
                       recursive=True):
        try:
            w, h, bpp, data = _imgc.decode(p)
        except Exception as e:
            continue
        if (w, h) != (400, 240):
            continue
        rgb = np.frombuffer(data, np.uint8).reshape(h, w, bpp)
        if bpp == 3:
            rgb = rgb[:, :, ::-1]
        else:
            rgb = np.repeat(rgb[:, :, :1], 3, axis=2)
        d = np.abs(rgb[:rows].astype(int) - ref).sum(axis=2)
        out.append((int((d > 60).sum()), rows * 400,
                    os.path.relpath(p, ROOT)))
    out.sort()
    for a, n, p in out:
        print('%6d/%6d  %s' % (a, n, p))


if __name__ == '__main__':
    main()
