"""Korrekter ETC1-Decode (PICA/Azahar-Layout) + Textur-Statistik/ASCII.

Ersetzt den fehlerhaften Decoder in tools/_title_etc1.py (Separate-Mode-
Felder lagen an falschen Bits -> Rauschen, mean ~110 statt ~30).

  python tools/out/title3d_etc1.py [--dump]
"""
import struct
import sys

import numpy as np

sys.path.insert(0, 'tools/out')
import title3d_lib as L
import _imgc

MOD = ((2, 8), (5, 17), (9, 29), (13, 42), (18, 60), (24, 80), (33, 106), (47, 183))
CH = ' .:-=+*#%@'


def c4(v):
    return v * 17


def c5(v):
    return (v << 3) | (v >> 2)


def s3(v):
    return v - 8 if v & 4 else v


def etc1_block(b, p, out, w, ox, oy):
    raw = int.from_bytes(b[p:p + 8], 'little')
    flip = (raw >> 32) & 1
    diffm = (raw >> 33) & 1
    t2 = (raw >> 34) & 7
    t1 = (raw >> 37) & 7
    if diffm:
        r1 = (raw >> 59) & 31
        g1 = (raw >> 51) & 31
        b1 = (raw >> 43) & 31
        r2 = max(0, min(31, r1 + s3((raw >> 56) & 7)))
        g2 = max(0, min(31, g1 + s3((raw >> 48) & 7)))
        b2 = max(0, min(31, b1 + s3((raw >> 40) & 7)))
        r1, g1, b1 = c5(r1), c5(g1), c5(b1)
        r2, g2, b2 = c5(r2), c5(g2), c5(b2)
    else:
        r1 = c4((raw >> 60) & 15)
        g1 = c4((raw >> 52) & 15)
        b1 = c4((raw >> 44) & 15)
        r2 = c4((raw >> 56) & 15)
        g2 = c4((raw >> 48) & 15)
        b2 = c4((raw >> 40) & 15)
    for y in range(4):
        for x in range(4):
            texel = 4 * x + y
            xx, yy = (y, x) if flip else (x, y)
            ti = t1 if xx < 2 else t2
            mod = MOD[ti][(raw >> texel) & 1]
            if (raw >> (16 + texel)) & 1:
                mod = -mod
            br, bg, bb = (r2, g2, b2) if xx >= 2 else (r1, g1, b1)
            o = ((oy + y) * w + ox + x) * 3
            out[o] = max(0, min(255, br + mod))
            out[o + 1] = max(0, min(255, bg + mod))
            out[o + 2] = max(0, min(255, bb + mod))


def decode(name):
    b = L.blob(name)
    w, h = struct.unpack_from('<HH', b, 0x10)
    s1, s2, sz = struct.unpack_from('<3i', b, 0x34)
    tab = _imgc.decomp(b[0x48:0x48 + s1])
    tex = _imgc.decomp(b[0x48 + s2:0x48 + s2 + sz])
    cols, rows = w // 8, h // 8
    n = cols * rows
    ents = struct.unpack_from('<%dH' % (len(tab) // 2), tab, 0)
    etc = bytearray(n * 32)
    for pp in range(n):
        e = ents[pp]
        if e == 0xFFFF or e * 32 + 32 > len(tex):
            continue
        etc[pp * 32:pp * 32 + 32] = tex[e * 32:e * 32 + 32]
    out = bytearray(w * h * 3)
    for ty in range(rows):
        for tx in range(cols):
            base = (ty * cols + tx) * 32
            for sy in range(2):
                for sx in range(2):
                    etc1_block(etc, base + (sy * 2 + sx) * 8, out, w,
                               tx * 8 + sx * 4, ty * 8 + sy * 4)
    return w, h, np.frombuffer(bytes(out), dtype=np.uint8).reshape(h, w, 3)


def ascii_art(a, w=64, h=20):
    l = a.astype(np.float32).mean(2)
    ih, iw = a.shape[:2]
    rows = []
    for y in range(h):
        y0, y1 = y * ih // h, max(y * ih // h + 1, (y + 1) * ih // h)
        s = ''
        for x in range(w):
            x0, x1 = x * iw // w, max(x * iw // w + 1, (x + 1) * iw // w)
            s += CH[min(9, int(l[y0:y1, x0:x1].mean() / 25.6))]
        rows.append(s)
    return '\n'.join(rows)


def main():
    for i in range(14):
        nm = '%03d.xi' % i
        w, h, a = decode(nm)
        l = a.astype(np.float32).mean(2)
        sm = float(np.abs(np.diff(l, axis=0)).mean() + np.abs(np.diff(l, axis=1)).mean())
        print('%s %dx%d mean=%.1f p50=%.0f p90=%.0f max=%.0f smooth=%.2f'
              % (nm, w, h, l.mean(), np.percentile(l, 50), np.percentile(l, 90), l.max(), sm))
        if '--dump' in sys.argv:
            print(ascii_art(a))


if __name__ == '__main__':
    main()
