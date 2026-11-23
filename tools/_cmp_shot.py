import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_bmp(p):
    d = open(p, 'rb').read()
    off = int.from_bytes(d[10:14], 'little')
    w = int.from_bytes(d[18:22], 'little', signed=True)
    h = int.from_bytes(d[22:26], 'little', signed=True)
    bpp = int.from_bytes(d[28:30], 'little')
    stride = ((w * bpp // 8) + 3) & ~3
    topdown = h < 0
    h = abs(h)
    px = []
    for y in range(h):
        row = y if topdown else (h - 1 - y)
        base = off + row * stride
        for x in range(w):
            b, g, r = d[base + x * 3], d[base + x * 3 + 1], d[base + x * 3 + 2]
            px.append((r, g, b))
    return w, h, px


def compare_region(w, px, x0, y0, rw, rh, ref_bytes, label):
    diff = 0
    maxd = 0
    n = min(len(ref_bytes) // 3, rw * rh)
    for y in range(rh):
        for x in range(rw):
            i = (y0 + y) * w + (x0 + x)
            r0, g0, b0 = px[i]
            j = (y * rw + x) * 3
            r1, g1, b1 = ref_bytes[j], ref_bytes[j + 1], ref_bytes[j + 2]
            dd = abs(r0 - r1) + abs(g0 - g1) + abs(b0 - b1)
            if dd:
                diff += 1
                if dd > maxd:
                    maxd = dd
    print(f'  {label}: pixels={n} diff_pixels={diff} maxdiff={maxd}')
    return n, diff


def main():
    a = sys.argv[1] if len(sys.argv) > 1 else 'out/shot.bmp'
    ref = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        ROOT, 'reports', 'menu_official.bin')
    ref_bottom = sys.argv[3] if len(sys.argv) > 3 else os.path.join(
        ROOT, 'reports', 'menu_official_bottom.bin')
    w, h, px = load_bmp(a)
    rb = open(ref, 'rb').read()
    print(f'bmp={a} {w}x{h}')
    if w == 400 and h == 480:
        total = 0
        _, d = compare_region(w, px, 0, 0, 400, 240, rb, 'top(0,0,400,240)')
        total += d
        if os.path.exists(ref_bottom):
            bb = open(ref_bottom, 'rb').read()
            _, d2 = compare_region(w, px, 40, 240, 320, 240, bb,
                                   'bottom(40,240,320,240)')
            total += d2
        else:
            print('  bottom: Referenz fehlt:', ref_bottom)
        print(f'gesamt diff_pixels={total}')
        return
    n = min(len(rb) // 3, w * h)
    diff = 0
    maxd = 0
    for i in range(n):
        r0, g0, b0 = px[i]
        r1, g1, b1 = rb[i * 3], rb[i * 3 + 1], rb[i * 3 + 2]
        dd = abs(r0 - r1) + abs(g0 - g1) + abs(b0 - b1)
        if dd:
            diff += 1
            maxd = max(maxd, dd)
    print(f'pixels={n} diff_pixels={diff} maxdiff={maxd}')
    for (x, y) in [(0, 0), (w // 2, h // 2), (w - 1, h - 1), (200, 100)]:
        i = y * w + x
        print((x, y), 'bmp=', px[i], 'ref=', tuple(rb[i * 3:i * 3 + 3]))


if __name__ == '__main__':
    main()
