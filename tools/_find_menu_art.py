import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import _imgc

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = open(os.path.join(ROOT, 'reports', 'menu_official.bin'), 'rb').read()


def decode_variant(path, mul, shift, sw):
    d = open(path, 'rb').read()
    s1 = struct.unpack_from('<i', d, 0x34)[0]
    s2 = struct.unpack_from('<i', d, 0x38)[0]
    sz = struct.unpack_from('<i', d, 0x3C)[0]
    w, h = struct.unpack_from('<hh', d, 0x10)
    bpp = d[0x0D] // 8
    t = _imgc.decomp(d[0x48:0x48 + s1])
    tex = _imgc.decomp(d[0x48 + s2:0x48 + s2 + sz])
    pic = bytearray()
    for pos in range(0, len(t), 2):
        e = struct.unpack_from('<H', t, pos)[0]
        idx = (e >> shift) if shift else e
        o = idx * mul
        if e == 0xFFFF or o + 192 > len(tex):
            pic.extend(b'\0' * 192)
        else:
            pic.extend(tex[o:o + 192])
    out = bytearray(w * h * bpp)
    for y in range(h):
        for x in range(w):
            if sw:
                sx, sy = _imgc.master_get(y * w + x, w)
            else:
                sx, sy = x, y
            o = (sy * w + sx) * bpp
            if o + bpp <= len(pic):
                out[(y * w + x) * bpp:(y * w + x + 1) * bpp] = pic[o:o + bpp]

    return w, h, bpp, out


def diff(out, w, h, bpp):
    n = min(w * h, len(REF) // 3)
    d = 0
    for i in range(n):
        if bpp >= 3:
            b, g, r = out[i * bpp], out[i * bpp + 1], out[i * bpp + 2]
        else:
            r = g = b = out[i * bpp]
        j = i * 3
        if abs(r - REF[j]) + abs(g - REF[j + 1]) + abs(b - REF[j + 2]) > 60:
            d += 1
    return d, n


def main():
    for art in range(21, 31):
        p = os.path.join(ROOT, 'out', 'fa_b', 'menu', 'art_%d.xi' % art)
        if not os.path.exists(p):
            continue
        best = None
        for mul in (8, 16, 64, 192):
            for shift in (0, 4):
                for sw in (0, 1):
                    w, h, bpp, out = decode_variant(p, mul, shift, sw)
                    d, n = diff(out, w, h, bpp)
                    if best is None or d < best[0]:
                        best = (d, mul, shift, sw, n)
        print('art_%d: best diff>60 %d/%d  mul=%d shift=%d sw=%d' % (
            art, best[0], best[4], best[1], best[2], best[3]))


if __name__ == '__main__':
    main()
