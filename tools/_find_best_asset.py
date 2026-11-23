import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _imgc

REF_TOP = open('reports/menu_official.bin', 'rb').read()
REF_BOT = open('reports/menu_official_bottom.bin', 'rb').read()


def decode_var(path, mul, shift, sw):
    d = open(path, 'rb').read()
    if d[:6] != b'IMGC00':
        return None
    s1 = struct.unpack_from('<i', d, 0x34)[0]
    s2 = struct.unpack_from('<i', d, 0x38)[0]
    sz = struct.unpack_from('<i', d, 0x3C)[0]
    w, h = struct.unpack_from('<hh', d, 0x10)
    bpp = d[0x0D] // 8
    if bpp < 3 or w <= 0 or h <= 0:
        return None
    t = _imgc.decomp(d[0x48:0x48 + s1])
    tex = _imgc.decomp(d[0x48 + s2:0x48 + s2 + sz])
    pic = bytearray()
    for pos in range(0, len(t), 2):
        e = struct.unpack_from('<H', t, pos)[0]
        idx = (e >> shift) if shift else e
        o = idx * mul
        if o + 192 <= len(tex):
            pic.extend(tex[o:o + 192])
        else:
            pic.extend(b'\0' * 192)
    out = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            if sw:
                sx, sy = _imgc.master_get(y * w + x, w)
            else:
                sx, sy = x, y
            o = (sy * w + sx) * 3
            if o + 3 <= len(pic):
                out[(y * w + x) * 3:(y * w + x + 1) * 3] = pic[o:o + 3]
    return w, h, out


def diff(out, w, h, ref):
    n = min(w * h, len(ref) // 3)
    d = 0
    for i in range(n):
        j = i * 3
        if abs(out[j] - ref[j]) + abs(out[j + 1] - ref[j + 1]) + abs(out[j + 2] - ref[j + 2]) > 60:
            d += 1
    return d, n


def main():
    best = []
    for p in glob.glob('out/fa_b/menu/*.xi'):
        d = open(p, 'rb').read(0x40)
        if d[:6] != b'IMGC00':
            continue
        w, h = struct.unpack_from('<hh', d, 0x10)
        ref = REF_TOP if (w, h) == (400, 240) else (REF_BOT if (w, h) == (320, 240) else None)
        if ref is None:
            continue
        b = None
        for mul in (8, 16, 32, 64, 96, 192):
            for shift in (0, 4):
                for sw in (0, 1):
                    r = decode_var(p, mul, shift, sw)
                    if not r:
                        continue
                    dd, n = diff(r[2], r[0], r[1], ref)
                    if b is None or dd < b[0]:
                        b = (dd, n, mul, shift, sw)
        best.append((b[0], os.path.basename(p), b[1], b[2], b[3], b[4]))
    best.sort(key=lambda z: z[0])
    for x in best[:15]:
        print('%6d/%6d  %-24s mul=%3d shift=%d sw=%d' % (
            x[0], x[2], x[1], x[3], x[4], x[5]))


if __name__ == '__main__':
    main()
