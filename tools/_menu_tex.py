import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _imgc

REF = open('reports/menu_official.bin', 'rb').read()
RW, RH = 400, 240
SW, SH = 100, 60


def ref_small():
    out = bytearray(SW * SH * 3)
    for y in range(SH):
        sy = y * RH // SH
        for x in range(SW):
            sx = x * RW // SW
            i = (sy * RW + sx) * 3
            out[(y * SW + x) * 3:(y * SW + x) * 3 + 3] = REF[i:i + 3]
    return bytes(out)


def small(rgb, w, h):
    out = bytearray(SW * SH * 3)
    for y in range(SH):
        sy = y * h // SH
        for x in range(SW):
            sx = x * w // SW
            i = (sy * w + sx) * 3
            out[(y * SW + x) * 3:(y * SW + x) * 3 + 3] = rgb[i:i + 3]
    return bytes(out)


def corr(a, b):
    n = len(a) // 3
    sa = sb = saa = sbb = sab = 0.0
    for i in range(0, n * 3, 3):
        x = (a[i] + a[i + 1] + a[i + 2]) / 3.0
        y = (b[i] + b[i + 1] + b[i + 2]) / 3.0
        sa += x
        sb += y
        saa += x * x
        sbb += y * y
        sab += x * y
    ma = sa / n
    mb = sb / n
    va = saa / n - ma * ma
    vb = sbb / n - mb * mb
    if va <= 0 or vb <= 0:
        return 0.0
    return (sab / n - ma * mb) / ((va ** 0.5) * (vb ** 0.5))


def main():
    path = sys.argv[1]
    off = int(sys.argv[2], 0)
    d = open(path, 'rb').read()[off:]
    s1 = struct.unpack_from('<i', d, 0x34)[0]
    s2 = struct.unpack_from('<i', d, 0x38)[0]
    sz = struct.unpack_from('<i', d, 0x3C)[0]
    w = struct.unpack_from('<H', d, 0x10)[0]
    h = struct.unpack_from('<H', d, 0x12)[0]
    bpp = d[0x0D] // 8
    tex = _imgc.decomp(d[0x48 + s2:0x48 + s2 + sz])
    print('w=%d h=%d bpp=%d tex=%d' % (w, h, bpp, len(tex)))
    rs = ref_small()
    for sw in (0, 1):
        rgb = bytearray(w * h * 3)
        for y in range(h):
            for x in range(w):
                px, py = x, y
                if sw:
                    px, py = _imgc.master_get(y * w + x, w)
                i = (py * w + px) * bpp
                if i + 3 <= len(tex):
                    b, g, r = tex[i], tex[i + 1], tex[i + 2]
                else:
                    b = g = r = 0
                o = (y * w + x) * 3
                rgb[o:o + 3] = bytes((r, g, b))
        print('sw=%d corr=%.4f' % (sw, corr(small(bytes(rgb), w, h), rs)))
        open('out/menu_tex_sw%d.raw' % sw, 'wb').write(bytes(rgb))


if __name__ == '__main__':
    main()
