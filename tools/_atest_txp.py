"""Dump .txp-Paletten und .xi-Header aus menu/title.xc."""
import struct
import sys

sys.path.insert(0, 'tools')
import _atest_titlexc as T

d = T.d
for o, size, name in ((0x00000DE4, 36, '000.txp'),
                      (0x00029548, 36, '002.txp'),
                      (0x0001BC30, 36, '001.txp'),
                      (0x00000E0C, 0x48, '000.xi')):
    print('===', name, 'off 0x%X size %d' % (o, size))
    for k in range(0, min(size, 96), 16):
        print('  %04X %s' % (k, ' '.join('%02X' % x
                                          for x in d[o + k:o + k + 16])))
