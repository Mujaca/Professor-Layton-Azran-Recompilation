"""Nutzt den Bottom-Capture als Ground Truth, um die Tile-Reihenfolge zu
bestimmen. Variante seq/ohne Swizzle ist am besten; hier Detailanalyse."""
import struct
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _imgc

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


def assemble(indices):
    tiles = np.zeros((len(indices), 8, 8, bpp), dtype=np.uint8)
    for k, e in enumerate(indices):
        if 0 <= e < NT:
            tiles[k] = SRC[int(e)].reshape(8, 8, bpp)
    return tiles


tiles = assemble(E)
cols, rows = w // 8, h // 8
img = tiles.reshape(rows, cols, 8, 8, bpp).transpose(0, 2, 1, 3, 4)
img = img.reshape(h, w, bpp)
rgb = np.zeros((h, w, 3), dtype=np.uint8)
rgb[:, :, 0] = img[:, :, 2]
rgb[:, :, 1] = img[:, :, 1]
rgb[:, :, 2] = img[:, :, 0]
d = np.abs(rgb.astype(np.int16) - CAP.astype(np.int16))
print('seq/linear: exact %.1f%% mean %.2f diff>60 %.1f%%'
      % ((d.sum(2) == 0).mean() * 100, d.mean(),
         (d.sum(2) > 60).mean() * 100))
eq = (d.sum(2) <= 30)
print('pixels with diff<=30: %.1f%%' % (eq.mean() * 100))
# pro Region
for y0 in range(0, 240, 40):
    r = (d[y0:y0 + 40].sum(2) > 60).mean() * 100
    print('  rows %3d-%3d diff>60 %.1f%%' % (y0, y0 + 39, r))
