"""Dekodiert den 512x256-Menue-Atlas mit dem beobachteten Tabellenformat
(e>>4 = Tile-Index, Tile = 192 Byte) und vergleicht mit der Referenz."""
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


def decode(path, off=0):
    d = open(path, 'rb').read()[off:]
    s1 = struct.unpack_from('<i', d, 0x34)[0]
    s2 = struct.unpack_from('<i', d, 0x38)[0]
    sz = struct.unpack_from('<i', d, 0x3C)[0]
    w = struct.unpack_from('<H', d, 0x10)[0]
    h = struct.unpack_from('<H', d, 0x12)[0]
    bpp = d[0x0D] // 8
    tile = struct.unpack_from('<H', d, 0x0E)[0]
    t = _imgc.decomp(d[0x48:0x48 + s1])
    tex = _imgc.decomp(d[0x48 + s2:0x48 + s2 + sz])
    pic = bytearray()
    for pos in range(0, len(t), 2):
        e = struct.unpack_from('<H', t, pos)[0]
        o = (e >> 4) * tile
        pic.extend(tex[o:o + tile] if o + tile <= len(tex) else b'\0' * tile)
    return w, h, bpp, pic


def main():
    w, h, bpp, pic = decode(sys.argv[1], int(sys.argv[2], 0) if len(sys.argv) > 2 else 0)
    print('decoded %dx%d bpp=%d pic=%d' % (w, h, bpp, len(pic)))
    w8 = (w + 7) & ~7
    rs = ref_small()
    results = []
    for sw in (0, 1):
        out = bytearray(w * h * bpp)
        for y in range(h):
            for x in range(w):
                if sw:
                    sx, sy = _imgc.master_get(y * w + x, w)
                else:
                    sx, sy = x, y
                o = (sy * w + sx) * bpp
                dst = (y * w + x) * bpp
                if o + bpp <= len(pic):
                    out[dst:dst + bpp] = pic[o:o + bpp]
        rgb = bytearray(w * h * 3)
        for i in range(w * h):
            if bpp >= 3:
                b, g, r = out[i * bpp], out[i * bpp + 1], out[i * bpp + 2]
            else:
                r = g = b = out[i * bpp]
            rgb[i * 3:i * 3 + 3] = bytes((r, g, b))
        c = corr(small(bytes(rgb), w, h), rs)
        print('sw=%d corr=%.4f' % (sw, c))
        results.append((c, sw, bytes(rgb)))
    c, sw, rgb = max(results)
    open('out/menu_candidate.raw', 'wb').write(rgb)
    print('best sw=%d corr=%.4f -> out/menu_candidate.raw' % (sw, c))


if __name__ == '__main__':
    main()
