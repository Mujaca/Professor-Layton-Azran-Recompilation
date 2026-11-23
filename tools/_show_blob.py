"""Druckt eine ASCII-Luminanzkarte eines IMGC-Blobs."""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _imgc


def main():
    path = sys.argv[1]
    off = int(sys.argv[2], 0)
    cols = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    d = open(path, 'rb').read()
    blob = d[off:]
    td = tempfile.mkdtemp()
    p = os.path.join(td, 'b.bin')
    open(p, 'wb').write(blob)
    w, h, bpp, pic = _imgc.decode(p)
    print('%s off=%d %dx%d bpp=%d' % (os.path.basename(path), off, w, h, bpp))
    rows = max(8, cols * h // (w * 2))
    chars = ' .:-=+*#%@'
    for gy in range(rows):
        y0 = gy * h // rows
        y1 = max(y0 + 1, (gy + 1) * h // rows)
        line = ''
        for gx in range(cols):
            x0 = gx * w // cols
            x1 = max(x0 + 1, (gx + 1) * w // cols)
            s = 0
            n = 0
            for y in range(y0, y1, 2):
                for x in range(x0, x1, 2):
                    i = (y * w + x) * bpp
                    if i + 3 <= len(pic) and bpp >= 3:
                        v = (pic[i] + pic[i + 1] + pic[i + 2]) // 3
                    elif i < len(pic):
                        v = pic[i]
                    else:
                        v = 0
                    s += v
                    n += 1
            line += chars[min(9, (s // max(1, n)) * 10 // 256)]
        print(line)


if __name__ == '__main__':
    main()
