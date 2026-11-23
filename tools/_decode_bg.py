"""Dekodiert Menue-Hintergrund-.xi (IMGC, Tabelle in 8-Byte-Einheiten,
ohne Z-Order) nach RGB24."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _imgc


def decode(path):
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
        o = e * 8
        if o + 192 <= len(tex):
            pic.extend(tex[o:o + 192])
        else:
            pic.extend(b'\0' * 192)
    return w, h, bpp, pic


def main():
    p = sys.argv[1]
    out = sys.argv[2]
    w, h, bpp, pic = decode(p)
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        if bpp >= 3:
            b, g, r = pic[i * bpp], pic[i * bpp + 1], pic[i * bpp + 2]
        else:
            r = g = b = pic[i * bpp]
        rgb[i * 3:i * 3 + 3] = bytes((r, g, b))
    open(out, 'wb').write(bytes(rgb))
    print('%s -> %s %dx%d' % (p, out, w, h))


if __name__ == '__main__':
    main()
