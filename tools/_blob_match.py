import glob
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


def blob_offsets(d):
    offs = []
    i = 0
    while True:
        j = d.find(b'IMGC', i)
        if j < 0:
            break
        offs.append(j)
        i = j + 1
    return offs


def decode_at(d, off):
    w, h = struct.unpack_from('<HH', d, off + 0x10)
    bpp = d[off + 0x0D] // 8
    blob = d[off:]
    import tempfile
    import subprocess
    td = tempfile.mkdtemp()
    p = os.path.join(td, 'b.bin')
    open(p, 'wb').write(blob)
    # _imgc.decode expects IMGC00 at 0
    return _imgc.decode(p)


def to_rgb(data, w, h, bpp):
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        if bpp >= 3:
            b = data[i * bpp]
            g = data[i * bpp + 1]
            r = data[i * bpp + 2]
        else:
            r = g = b = data[i * bpp]
        rgb[i * 3:i * 3 + 3] = bytes((r, g, b))
    return bytes(rgb)


def main():
    rs = ref_small()
    files = []
    for pat in sys.argv[1:] or ['out/x_menu/*.xi', 'out/x_title/*.xi', 'out/x_title_uk/*.xi']:
        files += glob.glob(pat)
    rows = []
    for path in files:
        d = open(path, 'rb').read()
        for off in blob_offsets(d):
            if off + 0x48 >= len(d):
                continue
            w, h = struct.unpack_from('<HH', d, off + 0x10)
            if w < 200 or h < 100:
                continue
            try:
                dw, dh, bpp, pic = decode_at(d, off)
            except Exception as e:
                print('  ! %s@%d: %s' % (os.path.basename(path), off, e))
                continue
            rgb = to_rgb(pic, dw, dh, bpp)
            c = corr(small(rgb, dw, dh), rs)
            rows.append((c, path, off, dw, dh, bpp))
            print('%6.3f  %-22s off=%-7d %dx%d bpp=%d' % (c, os.path.basename(path), off, dw, dh, bpp))
    rows.sort(key=lambda r: -r[0])
    print('--- best ---')
    for r in rows[:6]:
        print('%6.3f  %s off=%d %dx%d bpp=%d' % r)


if __name__ == '__main__':
    main()
