"""IMGC-Atlas: Tabellenordnung ist blockbasiert.
p = block*256 + row*16 + col; e = block*4096 + row*16 + col.
Quelle s = (e>>12)*256 + (e&0xFFF)."""
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


def load(path, off):
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
    e = [struct.unpack_from('<H', t, i)[0] for i in range(0, len(t), 2)]
    return w, h, bpp, tile, e, tex


def build(w, h, bpp, tile, e, tex, do_sw):
    cols = (w + 7) // 8
    rows = (h + 7) // 8
    tiles = bytearray(cols * rows * tile)

    def src(v):
        return (v >> 12) * 256 + (v & 0xFFF)

    for p in range(min(len(e), cols * rows)):
        block = p >> 8
        rem = p & 0xFF
        row = rem >> 4
        col = rem & 0xF
        bx = block % 4
        by = block // 4
        tx = bx * 16 + col
        ty = by * 16 + row
        if tx >= cols or ty >= rows:
            continue
        dst = ty * cols + tx
        s = src(e[p])
        if s * tile + tile <= len(tex):
            tiles[dst * tile:dst * tile + tile] = tex[s * tile:s * tile + tile]

    out = bytearray(w * h * bpp)
    for ty in range(rows):
        for tx in range(cols):
            blk = (ty * cols + tx) * tile
            for y in range(8):
                for x in range(8):
                    px, py = tx * 8 + x, ty * 8 + y
                    if do_sw:
                        px, py = _imgc.master_get(py * w + px, w)
                    if px >= w or py >= h:
                        continue
                    o = (y * 8 + x) * bpp
                    di = (py * w + px) * bpp
                    out[di:di + bpp] = tiles[blk + o:blk + o + bpp]
    return bytes(out)


def to_rgb(pic, w, h, bpp):
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        if bpp >= 3:
            b, g, r = pic[i * bpp], pic[i * bpp + 1], pic[i * bpp + 2]
        else:
            r = g = b = pic[i * bpp]
        rgb[i * 3:i * 3 + 3] = bytes((r, g, b))
    return bytes(rgb)


def main():
    w, h, bpp, tile, e, tex = load(sys.argv[1], int(sys.argv[2], 0) if len(sys.argv) > 2 else 0)
    rs = ref_small()
    best = None
    for do_sw in (0, 1):
        pic = build(w, h, bpp, tile, e, tex, do_sw)
        rgb = to_rgb(pic, w, h, bpp)
        c = corr(small(rgb, w, h), rs)
        print('sw=%d corr=%.4f' % (do_sw, c))
        if best is None or c > best[0]:
            best = (c, do_sw, rgb)
    print('BEST corr=%.4f sw=%d' % best[:2])
    open('out/menu_candidate3.raw', 'wb').write(best[2])


if __name__ == '__main__':
    main()
