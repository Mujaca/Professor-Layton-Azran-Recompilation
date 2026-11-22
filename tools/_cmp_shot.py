import sys

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
            b, g, r = dimura = d[base + x * 3], d[base + x * 3 + 1], d[base + x * 3 + 2]
            px.append((r, g, b))
    return w, h, px

def main():
    a = sys.argv[1] if len(sys.argv) > 1 else 'out/shot_engine_gpu.bmp'
    ref = sys.argv[2] if len(sys.argv) > 2 else 'reports/menu_official.bin'
    w, h, px = load_bmp(a)
    rb = open(ref, 'rb').read()
    n = min(len(rb) // 3, w * h)
    diff = 0
    maxd = 0
    for i in range(n):
        r0, g0, b0 = px[i]
        r1, g1, b1 = rb[i * 3], rb[i * 3 + 1], rb[i * 3 + 2]
        dd = abs(r0 - r1) + abs(g0 - g1) + abs(b0 - b1)
        if dd:
            diff += 1
            if dd > maxd:
                maxd = dd
    print(f'bmp={a} {w}x{h} pixels={n} diff_pixels={diff} maxdiff={maxd}')
    # sample corner + center
    for (x, y) in [(0, 0), (w // 2, h // 2), (w - 1, h - 1), (200, 100)]:
        i = y * w + x
        print((x, y), 'bmp=', px[i], 'ref=', tuple(rb[i*3:i*3+3]))

if __name__ == '__main__':
    main()
