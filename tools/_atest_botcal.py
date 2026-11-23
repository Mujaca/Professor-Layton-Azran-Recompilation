import sys

import numpy as np

sys.path.insert(0, 'tools')
import _atest_imgc_np as D

B = open('out/romfs/lt6/arc/lt6_b.fa', 'rb').read()
w, h, rgb, al = D.decode_buf(B, 0xCE43074)
cap = np.frombuffer(open('reports/menu_official_bottom.bin', 'rb').read(),
                    dtype=np.uint8).reshape(240, 320, 3)
vis = min(223, h)
a = rgb[:vis].astype(np.int16)
b = cap[:vis].astype(np.int16)
eq = (a == b).all(2)
d = np.abs(a - b)
print('asset %dx%d vis %d' % (w, h, vis))
print('exact equal %d/%d (%.1f%%)' % (int(eq.sum()), eq.size,
                                      eq.mean() * 100))
print('mean abs diff %.3f' % float(d.mean()))
print('diff>60 %d/%d' % (int((d.sum(2) > 60).sum()), d.shape[0] * d.shape[1]))
