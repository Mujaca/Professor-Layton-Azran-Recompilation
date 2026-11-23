import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _decode_blob

REF = open('reports/menu_official.bin', 'rb').read()
RW, RH = 400, 240


def score_scaled(out, w, h, bpp):
    dd = 0
    n = 0
    for y in range(0, RH, 4):
        sy = min(h - 1, y * h // RH)
        for x in range(0, RW, 4):
            sx = min(w - 1, x * w // RW)
            i = (sy * w + sx) * bpp
            j = (y * RW + x) * 3
            if abs(out[i] - REF[j]) + abs(out[i + 1] - REF[j + 1]) + abs(out[i + 2] - REF[j + 2]) > 60:
                dd += 1
            n += 1
    return dd, n


def main():
    for path, off in [('out/x_menu/000.xi', 0),
                      ('out/x_title_uk/001.xi', 961796),
                      ('out/x_title_uk/002.xi', 950276),
                      ('out/x_title_uk/003.xi', 968216)]:
        best = None
        for mul in (8, 16, 32, 64, 256):
            for shift in (0, 4):
                for sw in (0, 1):
                    try:
                        w, h, bpp, out = _decode_blob.decode_blob(path, off, mul, shift, sw)
                    except Exception:
                        continue
                    if (w, h) != (512, 256):
                        continue
                    dd, n = score_scaled(out, w, h, bpp)
                    if best is None or dd < best[0]:
                        best = (dd, n, mul, shift, sw)
        print('%s off=%d best %s' % (path, off, best))


if __name__ == '__main__':
    main()
