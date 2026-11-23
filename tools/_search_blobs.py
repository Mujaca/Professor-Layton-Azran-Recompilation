import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _decode_blob

REF = open('reports/menu_official.bin', 'rb').read()
RW, RH = 400, 240


def blobs(path):
    d = open(path, 'rb').read()
    out = []
    i = 0
    while True:
        j = d.find(b'IMGC00', i)
        if j < 0:
            break
        out.append(j)
        i = j + 1
    return out


def main():
    files = sys.argv[1:]
    results = []
    for path in files:
        for off in blobs(path):
            best = None
            for mul in (8, 16, 32, 64, 256):
                for shift in (0, 4):
                    for sw in (0, 1):
                        try:
                            w, h, bpp, out = _decode_blob.decode_blob(path, off, mul, shift, sw)
                        except Exception:
                            continue
                        if w < RW or h < RH:
                            continue
                        for y0 in range(0, h - RH + 1, 16):
                            for x0 in range(0, w - RW + 1, 16):
                                dd = 0
                                n = 0
                                for y in range(0, RH, 8):
                                    base = ((y0 + y) * w + x0) * bpp
                                    rb = (y * RW) * 3
                                    for x in range(0, RW, 8):
                                        i = base + x * bpp
                                        j = rb + x * 3
                                        if abs(out[i] - REF[j]) + abs(out[i + 1] - REF[j + 1]) + abs(out[i + 2] - REF[j + 2]) > 80:
                                            dd += 1
                                        n += 1
                                if best is None or dd < best[0]:
                                    best = (dd, n, x0, y0, mul, shift, sw, w, h)
            if best:
                results.append((best[0], path, off, best))
                print('%-34s off=%9d best=%d/%d at %d,%d mul=%d shift=%d sw=%d %dx%d' % (
                    os.path.basename(path), off, best[0], best[1], best[2], best[3],
                    best[4], best[5], best[6], best[7], best[8]))
                sys.stdout.flush()
    results.sort()
    print('--- TOP 5 ---')
    for r in results[:5]:
        print('%d %s off=%d %s' % (r[0], os.path.basename(r[1]), r[2], r[3]))


if __name__ == '__main__':
    main()
