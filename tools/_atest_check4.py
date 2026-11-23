"""Prueft Plausibilitaet der 4bpp-Dekodierung (Nibble-Reihenfolge/Swizzle)."""
import struct
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _imgc
import _atest_titlexc as T
import _atest_imgc_np as D

o = 0x00000E0C
d = T.d
w, h = struct.unpack_from('<hh', d, o + 0x10)
s1, s2, szbef = struct.unpack_from('<iii', d, o + 0x34)
tab = _imgc.decomp(d[o + 0x48:o + 0x48 + s1])
tex = _imgc.decomp(d[o + 0x48 + s2:o + 0x48 + s2 + szbef])
e = np.frombuffer(tab, dtype='<u2').astype(np.int64)
bpt = 32
ntile = len(tex) // bpt
src = np.frombuffer(tex, dtype=np.uint8)[:ntile * bpt].reshape(ntile, bpt)
tiles = np.zeros((len(e), bpt), dtype=np.uint8)
ok = (e < ntile)
tiles[ok] = src[e[ok]]
pic = tiles.reshape(-1)
sid = D.swizzle_src(w, h).reshape(-1)
b = pic[np.clip(sid, 0, len(pic) - 1)]


def stats(vals, namehip):
    v = vals.astype(np.int16).reshape(h, w)
    dx = np.abs(np.diff(v, axis=1)).mean()
    dy = np.abs(np.diff(v, axis=0)).mean()
    print('%-22s dx=%.2f dy=%.2f' % (namehip, dx, dy))


stats(b & 0x0F, 'low-nibble (swizzle)')
stats((b >> 4) & 0x0F, 'high-nibble (swizzle)')
# ohne Swizzle
b2 = pic[np.clip(np.arange(w * h), 0, len(pic) - 1)]
stats(b2 & 0x0F, 'low-nibble (linear)')
