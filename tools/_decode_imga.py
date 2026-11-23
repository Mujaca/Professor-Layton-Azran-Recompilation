import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _imgc


def decode(path, mul, shift, sw):
    d = open(path, 'rb').read()
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
        if o + tile <= len(tex):
            pic.extend(tex[o:o + tile])
        else:
            pic.extend(b'\0' * tile)
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
    for mul in (8, 16, 32, 64, 256):
        for shift in (0, 4):
            for sw in (0, 1):
                w, h, bpp, out = decode(p, mul, shift, sw)
                rgb = bytearray(w * h * 3)
                for i in range(w * h):
                    rgb[i * 3:i * 3 + 3] = out[i * bpp:i * bpp + 3]
                fn = 'out/imga_%d_%d_%d.ppm' % (mul, shift, sw)
                open(fn, 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + bytes(rgb))
                print(fn, w, h)


if __name__ == '__main__':
    main()
