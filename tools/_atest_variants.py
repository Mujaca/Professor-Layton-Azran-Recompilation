import struct
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _imgc
import _atest_imgc_np as D

B = open('out/romfs/lt6/arc/lt6_b.fa', 'rb').read()
OFF = 0xCE43074
CAP = np.frombuffer(open('reports/menu_official_bottom.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 320, 3)
o = OFF
s1, s2, sz = struct.unpack_from('<iii', B, o + 0x34)
w, h = struct.unpack_from('<hh', B, o + 0x10)
bpp = B[o + 0x0D] // 8
bpt = 64 * bpp
TAB = _imgc.decomp(B[o + 0x48:o + 0x48 + s1])
TEX = _imgc.decomp(B[o + 0x48 + s2:o + 0x48 + s2 + sz])
E = np.frombuffer(TAB, dtype='<u2').astype(np.int64)
NT = len(TEX) // bpt
SRC = np.frombuffer(TEX, dtype=np.uint8)[:NT * bpt].reshape(NT, bpt)


def to_rgb(pic, w, h):
    px = np.frombuffer(pic, dtype=np.uint8).reshape(-1, bpp)[:, :3]
    out = np.zeros((w * h, 3), dtype=np.uint8)
    out[:, 0] = px[:, 2]
    out[:, 1] = px[:, 1]
    out[:, 2] = px[:, 0]
    return out.reshape(h, w, 3)


def build(mode, swz):
    cols = (w + 7) // 8
    rows = (h + 7) // 8
    tiles = np.zeros((cols * rows, bpt), dtype=np.uint8)
    if mode == 'seq':
        for k in range(min(len(E), cols * rows)):
            e = E[k]
            if 0 <= e < NT:
                tiles[k] = SRC[e]
    else:
        for p in range(min(len(E), cols * rows)):
            e = int(E[p])
            block = p >> 8
            rem = p & 0xFF
            row = rem >> 4
            col = rem & 0xF
            tx = (block % 4) * 16 + col
            ty = (block // 4) * 16 + row
            if tx >= cols or ty >= rows:
                continue
            s = (e >> 12) * 256 + (e & 0xFFF)
            if 0 <= s < NT:
                tiles[ty * cols + tx] = SRC[s]
    # Bild aus Tiles zusammensetzen
    out = np.zeros((h, w, bpp), dtype=np.uint8)
    for ty in range(rows):
        for tx in range(cols):
            t = tiles[ty * cols + tx].reshape(8, 8, bpp)
            out[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8] = t
    if swz:
        idx = D.swizzle_src(w, h).reshape(-1)
        out = out.reshape(-1, bpp)[idx].reshape(h, w, bpp)
    rgb = np.zeros((h, w, 3), dtype=np.uint8)
    rgb[:, :, 0] = out[:, :, 2]
    rgb[:, :, 1] = out[:, :, 1]
    rgb[:, :, 2] = out[:, :, 0]
    return rgb


def score(rgb, name):
    a = rgb[:223].astype(np.float64)
    b = CAP[:223].astype(np.float64)
    cs = [float(np.corrcoef(a[:, :, c].ravel(), b[:, :, c].ravel())[0, 1])
          for c in range(3)]
    print('%-24s corr %s diff>60 %.1f%%'
          % (name, ' '.join('%.3f' % c for c in cs),
             float((np.abs(a - b).sum(2) > 60).mean() * 100)))


for mode in ('seq', 'block'):
    for swz in (0, 1):
        rgb = build(mode, swz)
        score(rgb, '%s swizzle=%d' % (mode, swz))
