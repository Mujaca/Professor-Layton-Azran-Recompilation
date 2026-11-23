"""Scannt ALLE XPCK-Packs in den .fa-Archiven nach IMGCs 400x240/320x240
und vergleicht sie mit den Captures (findet den echten Top-/Bottom-Atlas).

  python tools/scan_xpck.py
"""
import os
import struct
import sys
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _atest_imgc_np as D

FAS = ['out/romfs/lt6/arc/lt6_a.fa',
       'out/romfs/lt6/arc/lt6_b.fa',
       'out/romfs/lt6/arc/lt6_uk.fa']
TOP = np.frombuffer(open(os.path.join(ROOT, 'reports', 'menu_official.bin'),
                         'rb').read(), np.uint8).reshape(240, 400, 3).astype(int)
BOT = np.frombuffer(open(os.path.join(
    ROOT, 'reports', 'menu_official_bottom.bin'), 'rb').read(),
    np.uint8).reshape(240, 320, 3).astype(int)
res = []


def scan_xpck(d, base):
    if base + 0x10 > len(d):
        return
    fc1, fc2 = d[base + 4], d[base + 5]
    count = ((fc2 & 0xF) << 8) | fc1
    if count == 0 or count > 4096:
        return
    info_off = struct.unpack_from('<H', d, base + 6)[0] << 2
    data_off = struct.unpack_from('<H', d, base + 0xA)[0] << 2
    for i in range(count):
        e = base + info_off + i * 12
        if e + 12 > len(d):
            return
        h, no, t1, t2, z1, z2 = struct.unpack_from('<IHHHBB', d, e)
        fo = base + data_off + (((z1 << 16) | t1) << 2)
        if d[fo:fo + 6] != b'IMGC00':
            continue
        w, hh = struct.unpack_from('<hh', d, fo + 0x10)
        if (w, hh) not in ((400, 240), (320, 240)):
            continue
        try:
            r = D.decode_buf(d, fo)
        except Exception:
            continue
        if not r:
            continue
        ww, hhh, rgb, al = r
        if (ww, hhh) != (w, hh):
            continue
        if (w, hh) == (400, 240):
            dd = np.abs(rgb.astype(int) - TOP)
            rows = 226
        else:
            dd = np.abs(rgb.astype(int) - BOT)
            rows = 223
        res.append((int((dd.sum(axis=2)[:rows] > 60).sum()), fo, w, hh,
                    float(dd.mean())))


def main():
    for rel in FAS:
        p = os.path.join(ROOT, rel)
        d = open(p, 'rb').read()
        t = time.time()
        i = 0
        n = 0
        while True:
            j = d.find(b'XPCK', i)
            if j < 0:
                break
            scan_xpck(d, j)
            n += 1
            i = j + 4
        print('%s: %d XPCK, %.1fs, Treffer gesamt %d'
              % (rel, n, time.time() - t, len(res)))
    res.sort()
    for r in res[:12]:
        tot = 226 * 400 if r[2] == 400 else 223 * 320
        print('   diff>60 %6d/%6d  @%08X %dx%d mean %.2f'
              % (r[0], tot, r[1], r[2], r[3], r[4]))


if __name__ == '__main__':
    main()
