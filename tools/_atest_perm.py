"""Bestimmt die Tile-Permutation: Capture-Tile -> Quell-Tile (SAD)."""
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
SRC = np.frombuffer(TEX, dtype=np.uint8)[:NT * bpt].reshape(NT, 8, 8, bpp)
# in RGB
SRC_RGB = SRC[:, :, :, ::-1].reshape(NT, bpt)
CAPT = CAP.reshape(h // 8, 8, w // 8, 8, 3).transpose(0, 2, 1, 3, 4)
CAPT = CAPT.reshape(-1, bpt).astype(np.int32)
SR = SRC_RGB.reshape(NT, bpt).astype(np.int32)
# SAD = sum |a-b| = sum a - ... nutze Distanz ueber Quadrate + lineare Terme
A2 = (SR ** 2).sum(1)
B2 = (CAPT ** 2).sum(1)
D = A2[None, :] + B2[:, None] - 2 * (CAPT @ SR.T)
k = np.argmin(D, axis=1)
best = D[np.arange(len(CAPT)), k]
print('tiles capture', len(CAPT), 'src', NT)
print('best SSD^0.5 : min %.1f median %.1f max %.1f'
      % (np.sqrt(best).min(), np.sqrt(np.median(best)), np.sqrt(best).max()))
print('E[k] / k Beispiele (erste 40 Capture-Tiles):')
print('k :', k[:40].tolist())
print('E[k]:', E[k[:40]].tolist())
ok = np.sqrt(best) < 10
print('tiles mit SSD^0.5<10:', int(ok.sum()), 'von', len(k))
print('davon k == E[k]:', int((k[ok] == E[k[ok]]).sum()))
