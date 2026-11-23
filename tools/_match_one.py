import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _decode_blob

REF = open('reports/menu_official_bottom.bin', 'rb').read()
RW, RH = 320, 240
p = sys.argv[1]
best = None
for mul in (8, 16, 32, 64, 192, 256):
    for shift in (0, 4):
        for sw in (0, 1):
            w, h, bpp, out = _decode_blob.decode_blob(p, 0, mul, shift, sw)
            dd = 0
            n = 0
            for y in range(0, RH, 4):
                for x in range(0, RW, 4):
                    i = (y * w + x) * bpp
                    j = (y * RW + x) * 3
                    if abs(out[i] - REF[j]) + abs(out[i + 1] - REF[j + 1]) + abs(out[i + 2] - REF[j + 2]) > 60:
                        dd += 1
                    n += 1
            if best is None or dd < best[0]:
                best = (dd, n, mul, shift, sw)
print('best', best)
