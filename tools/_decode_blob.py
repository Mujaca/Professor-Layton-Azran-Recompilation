import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _imgc


def decode_blob(path, off, mul, shift, sw):
    d = open(path, 'rb').read()[off:]
    s1 = struct.unpack_from('<i', d, 0x34)[0]
    s2 = struct.unpack_from('<i', d, 0x38)[0]
    sz = struct.unpack_from('<i', d, 0x3C)[0]
    w, h = struct.unpack_from('<hh', d, 0x10)
    bpp = d[0x0D] // 8
    tile = 64 * bpp
    t = _imgc.decomp(d[0x48:0x48 + s1])
    tex = _imgc.decomp(d[0x48 + s2:0x48 + s2 + sz])
    pic = bytearray()
    for pos in range(0, len(t), 2):
        e = struct.unpack_from('<H', t, pos)[0]
        idx = (e >> shift) if shift else e
        o = idx * mul
        pic.extend(tex[o:o + tile] if o + tile <= len(tex) else b'\0' * tile)
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


def main():
    p = sys.argv[1]
    off = int(sys.argv[2])
    ref = open('reports/menu_official.bin', 'rb').read()
    RW, RH = 400, 240
    best = None
    for mul in (8, 16, 32, 64, 256):
        for shift in (0, 4):
            for sw in (0, 1):
                w, h, bpp, out = decode_blob(p, off, mul, shift, sw)
                if w < RW or h < RH:
                    continue
                for y0 in range(0, h - RH + 1, 8):
                    for x0 in range(0, w - RW + 1, 8):
                        dd = 0
                        n = 0
                        for y in range(0, RH, 8):
                            base = ((y0 + y) * w + x0) * 3
                            rb = (y * RW) * 3
                            for x in range(0, RW, 8):
                                i = base + x * bpp
                                j = rb + x * 3
                                if abs(out[i] - ref[j]) + abs(out[i+1] - ref[j+1]) + abs(out[i+2] - ref[j+2]) > 80:
                                    dd += 1
                                n += 1
                        if best is None or dd < best[0]:
                            best = (dd, n, x0, y0, mul, shift, sw)
    print('best', best)


if __name__ == '__main__':
    main()
