"""Decode title_b.xa/000.xi (RGBA) + speichere Atlas/Capture/BG als PNG."""
import os
import struct
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _atest_xpck as X
import _atest_imgc_np as D
import _imgc

FA = os.path.join(ROOT, 'out/romfs/lt6/arc/lt6_uk.fa')
XA_OFF, XA_LEN = 0x01526DC0, 0x9278


def main():
    d = open(FA, 'rb').read()
    cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
    e = [x for x in ents if x['name'] == '000.xi'][0]
    o = XA_OFF + e['off']
    w, h, rgb, a = D.decode_buf(d, o)
    print('000.xi %dx%d bits=%d alpha=%s' % (w, h, d[o + 0x0D],
                                             None if a is None else
                                             (a.min(), a.max(),
                                              int((a > 0).sum()))))
    rgba = np.empty((h, w, 4), np.uint8)
    rgba[..., :3] = rgb
    rgba[..., 3] = 255 if a is None else a
    Image.fromarray(rgba, 'RGBA').save(os.path.join(ROOT, 'tools/out/anmc_atlas.png'))
    # reference + bg
    cap = np.frombuffer(open(os.path.join(ROOT, 'reports/menu_official_bottom.bin'),
                             'rb').read(), dtype=np.uint8).reshape(240, 320, 3)
    Image.fromarray(cap, 'RGB').save(os.path.join(ROOT, 'tools/out/anmc_capture.png'))
    bg = np.frombuffer(open(os.path.join(ROOT, 'out/title_b_bg.bin'),
                            'rb').read(), dtype=np.uint8).reshape(240, 320, 3)
    Image.fromarray(bg, 'RGB').save(os.path.join(ROOT, 'tools/out/anmc_bg.png'))
    # residual-colored PNG
    dd = np.abs(cap.astype(np.int16) - bg.astype(np.int16)).max(axis=2)
    Image.fromarray(np.clip(dd * 3, 0, 255).astype(np.uint8), 'L').save(
        os.path.join(ROOT, 'tools/out/anmc_residual.png'))
    print('written tools/out/anmc_atlas.png, anmc_capture.png, anmc_bg.png, '
          'anmc_residual.png')


if __name__ == '__main__':
    main()
