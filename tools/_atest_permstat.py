import struct
import sys

import numpy as np

sys.path.insert(0, 'tools')
import _imgc
import _atest_botscan as BS

CAP = BS.CAP
B = open('out/romfs/lt6/arc/lt6_b.fa', 'rb').read()
o = 0xCE43074
s1, s2, sz = struct.unpack_from('<iii', B, o + 0x34)
w, h = struct.unpack_from('<hh', B, o + 0x10)
bpp = B[o + 0x0D] // 8
bpt = 64 * bpp
TAB = _imgc.decomp(B[o + 0x48:o + 0x48 + s1])
TEX = _imgc.decomp(B[o + 0x48 + s2:o + 0x48 + s2 + sz])
E = np.frombuffer(TAB, dtype='<u2', count=len(TAB) // 2).astype(np.int64)
NT = len(TEX) // bpt
SR = np.frombuffer(TEX, dtype=np.uint8)[:NT * bpt].reshape(NT, 8, 8, bpp)
SR = SR[:, :, :, ::-1].reshape(NT, bpt).astype(np.int32)
CT = CAP.reshape(h // 8, 8, w // 8, 8, 3).transpose(0, 2, 1, 3, 4)
CT = CT.reshape(-1, bpt).astype(np.int32)
Dm = (SR ** 2).sum(1)[None, :] + (CT ** 2).sum(1)[:, None] - 2 * (CT @ SR.T)
k = np.argmin(Dm, axis=1)
print('tiles', len(k), 'k==E[k]:', int((k == E[k]).sum()))
print('k in E[:len]:', int((k < len(E)).sum()))
# Anteil, wo k relativ zu E[k] verschoben ist
diff = k - np.clip(E, 0, len(k) - 1)
import collections
print('haeufigste k-E:', collections.Counter(diff.tolist()).most_common(8))
