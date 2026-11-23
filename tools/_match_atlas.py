import glob
import sys

REF = open('reports/menu_official.bin', 'rb').read()
RW, RH = 400, 240


def load_ppm(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def main():
    best = []
    for p in glob.glob('out/imga_*.ppm'):
        w, h, px = load_ppm(p)
        bm = None
        for y0 in range(0, h - RH + 1, 16):
            for x0 in range(0, w - RW + 1, 16):
                d = 0
                n = 0
                for y in range(0, RH, 8):
                    base = ((y0 + y) * w + x0) * 3
                    rb = (y * RW) * 3
                    for x in range(0, RW, 8):
                        i = base + x * 3
                        j = rb + x * 3
                        if abs(px[i] - REF[j]) + abs(px[i+1] - REF[j+1]) + abs(px[i+2] - REF[j+2]) > 80:
                            d += 1
                        n += 1
                if bm is None or d < bm[0]:
                    bm = (d, n, x0, y0)
        best.append((bm[0], bm[1], bm[2], bm[3], p))
    best.sort()
    for x in best[:10]:
        print('%6d/%6d off=%d,%d %s' % (x[0], x[1], x[2], x[3], x[4]))


if __name__ == '__main__':
    main()
