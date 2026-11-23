"""Dekodiert ETC1-IMGC (imageFormat 27/28) inkl. Tile-Tabelle und Swizzle."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _imgc

REF = open('reports/menu_official.bin', 'rb').read()
RW, RH = 400, 240
SW, SH = 100, 60

MODS = ((2, 8, -2, -8), (5, 17, -5, -17), (9, 29, -9, -29),
        (13, 42, -13, -42), (18, 60, -18, -60), (24, 80, -24, -80),
        (33, 106, -33, -106), (47, 183, -47, -183))


def expand4(v):
    return (v << 4) | v


def etc1_block(buf, p, out, w, ox, oy):
    high = int.from_bytes(buf[p:p + 4], 'big')
    low = int.from_bytes(buf[p + 4:p + 8], 'big')
    diff = (high >> 1) & 1
    flip = high & 1
    table = (high >> 5) & 7
    if diff:
        def s5(v):
            v &= 0x1F
            return v - 32 if v & 0x10 else v

        r1 = (high >> 27) & 0x1F
        g1 = (high >> 19) & 0x1F
        b1 = (high >> 11) & 0x1F
        r2 = r1 + s5(high >> 3)
        g2 = g1 + s5(low >> 27)
        b2 = b1 + s5(low >> 19)
        r1 = (r1 << 3) | (r1 >> 2)
        g1 = (g1 << 3) | (g1 >> 2)
        b1 = (b1 << 3) | (b1 >> 2)
        r2 = max(0, min(31, r2))
        g2 = max(0, min(31, g2))
        b2 = max(0, min(31, b2))
        r2 = (r2 << 3) | (r2 >> 2)
        g2 = (g2 << 3) | (g2 >> 2)
        b2 = (b2 << 3) | (b2 >> 2)
    else:
        r1 = expand4((high >> 28) & 0xF)
        g1 = expand4((high >> 20) & 0xF)
        b1 = expand4((high >> 12) & 0xF)
        r2 = expand4((high >> 4) & 0xF)
        g2 = expand4((low >> 28) & 0xF)
        b2 = expand4((low >> 20) & 0xF)
    for i in range(16):
        x = i // 4
        y = i % 4
        msb = (low >> (16 + i)) & 1
        lsb = (low >> i) & 1
        m = MODS[table][(msb << 1) | lsb]
        sub2 = (x >= 2) if flip else (y >= 2)
        br, bg, bb = (r2, g2, b2) if sub2 else (r1, g1, b1)
        r = max(0, min(255, br + m))
        g = max(0, min(255, bg + m))
        b = max(0, min(255, bb + m))
        o = ((oy + y) * w + ox + x) * 3
        out[o] = r
        out[o + 1] = g
        out[o + 2] = b


def load(path, off):
    d = open(path, 'rb').read()[off:]
    s1, s2, sz = struct.unpack_from('<iii', d, 0x34)
    w, h = struct.unpack_from('<HH', d, 0x10)
    fmt = d[0x0A]
    bitdepth = d[0x0D]
    tile = struct.unpack_from('<H', d, 0x0E)[0]
    table = _imgc.decomp(d[0x48:0x48 + s1])
    tex = _imgc.decomp(d[0x48 + s2:0x48 + s2 + sz])
    return w, h, fmt, bitdepth, tile, table, tex


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


def build(w, h, tile, table, tex, order):
    cols = (w + 7) // 8
    rows = (h + 7) // 8
    n = cols * rows
    etc = bytearray(n * tile)
    ents = struct.unpack_from('<%dH' % (len(table) // 2), table, 0)
    for p in range(min(n, len(ents))):
        e = ents[p]
        if e == 0xFFFF:
            continue
        s = e
        if order == 1:
            s = (e >> 12) * 256 + (e & 0xFFF)
        if s * tile + tile > len(tex):
            continue
        etc[p * tile:p * tile + tile] = tex[s * tile:s * tile + tile]
    out = bytearray(w * h * 3)
    for ty in range(rows):
        for tx in range(cols):
            base = (ty * cols + tx) * tile
            for sy in range(2):
                for sx in range(2):
                    p = base + (sy * 2 + sx) * 8
                    etc1_block(etc, p, out, w, tx * 8 + sx * 4, ty * 8 + sy * 4)
    if order == 2:
        sw = bytearray(w * h * 3)
        for y in range(h):
            for x in range(w):
                px, py = _imgc.master_get(y * w + x, w)
                o = (y * w + x) * 3
                q = (py * w + px) * 3
                sw[o:o + 3] = out[q:q + 3]
        out = sw
    return bytes(out)


def main():
    path = sys.argv[1]
    off = int(sys.argv[2], 0)
    w, h, fmt, bitdepth, tile, table, tex = load(path, off)
    print('%s off=%d %dx%d fmt=%d bitdepth=%d tile=%d table=%d tex=%d'
          % (os.path.basename(path), off, w, h, fmt, bitdepth, tile,
             len(table), len(tex)))
    rs = ref_small()
    best = None
    for order in (0, 1, 2):
        rgb = build(w, h, tile, table, tex, order)
        c = corr(small(rgb, w, h), rs)
        print('order=%d corr=%.4f' % (order, c))
        if best is None or c > best[0]:
            best = (c, order, rgb)
    print('BEST corr=%.4f order=%d' % best[:2])
    open('out/title_candidate.raw', 'wb').write(best[2])


if __name__ == '__main__':
    main()
