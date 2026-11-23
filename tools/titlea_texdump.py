"""Dump der 5 IMGC-Texturen aus menu/uk/title_a.xa als PNG (+ Alpha-Statistik)."""
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc
import _atest_imgc_np as D
import _atest_xpck as X

FA = os.path.join(ROOT, 'out', 'romfs', 'lt6', 'arc', 'lt6_uk.fa')
XA_OFF, XA_LEN = 0x01428314, 0xFEAA8

d = open(FA, 'rb').read()
cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
os.makedirs(os.path.join(ROOT, 'tools', 'out', 'titlea'), exist_ok=True)
for e in ents:
    if e['magic'] != 'IMGC00':
        continue
    o = XA_OFF + e['off']
    r = D.decode_buf(d, o)
    if r is None:
        print(e['name'], 'decode fail')
        continue
    w, h, rgb, al = r
    a = al if al is not None else np.full((h, w), 255, np.uint8)
    print('%-8s abs=%08X %4dx%-4d rgbmean=%s alpha mean=%.1f frac0=%.2f frac255=%.2f'
          % (e['name'], o, w, h, np.round(rgb.mean(axis=(0, 1)), 1),
             a.mean(), (a == 0).mean(), (a == 255).mean()))
    Image.fromarray(rgb).save(os.path.join(ROOT, 'tools/out/titlea',
                                           e['name'].replace('.', '_') + '_rgb.png'))
    Image.fromarray(a).save(os.path.join(ROOT, 'tools/out/titlea',
                                         e['name'].replace('.', '_') + '_a.png'))
