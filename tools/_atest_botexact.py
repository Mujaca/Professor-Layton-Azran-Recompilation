"""Gegenprobe: exakte 8x8-Bloecke title_b_bg -> Bottom-Capture."""
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_botscan as BS

CAP = BS.CAP
B = open('out/romfs/lt6/arc/lt6_b.fa', 'rb').read()
img = BS.decode_linear(B, 216281204)
keys = {}
for y in range(0, 240 - 7):
    for x in range(0, 320 - 7):
        b = CAP[y:y + 8, x:x + 8]
        if (b == b[0, 0]).all():
            continue
        keys.setdefault(b.tobytes(), (x, y))
print('ref keys', len(keys))
hit = tot = 0
for y in range(0, 240 - 7, 8):
    for x in range(0, 320 - 7, 8):
        b = img[y:y + 8, x:x + 8]
        if (b == b[0, 0]).all():
            continue
        tot += 1
        if b.tobytes() in keys:
            hit += 1
print('title_b_bg: exact non-uniform 8x8 hits %d/%d (%.1f%%)'
      % (hit, tot, hit / max(tot, 1) * 100))
# 4x4 und 2x2
for TS in (4, 2):
    k2 = set()
    for y in range(0, 240 - TS + 1):
        for x in range(0, 320 - TS + 1):
            b = CAP[y:y + TS, x:x + TS]
            if (b == b[0, 0]).all():
                continue
            k2.add(b.tobytes())
    h2 = t2 = 0
    for y in range(0, 240 - TS + 1, TS):
        for x in range(0, 320 - TS + 1, TS):
            b = img[y:y + TS, x:x + TS]
            if (b == b[0, 0]).all():
                continue
            t2 += 1
            if b.tobytes() in k2:
                h2 += 1
    print('  %dx%d: %d/%d (%.1f%%)' % (TS, TS, h2, t2, h2 / max(t2, 1) * 100))
