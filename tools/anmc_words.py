"""ANMC00-Records als Worttabelle dumpen (Analyse).

Aufruf: python tools/anmc_words.py 0x160 0x250 0x4E0
"""
import struct
import sys

d = open('tools/out/anmc_res.bin', 'rb').read()


def show(off, n=96):
    print('=== %04X' % off)
    for k in range(off, off + n, 4):
        raw = d[k:k + 4]
        u = struct.unpack('<I', raw)[0]
        i = struct.unpack('<i', raw)[0]
        f = struct.unpack('<f', raw)[0]
        print('%04X  %08X  int %11d  float %12.5f' % (k, u, i, f))


for a in sys.argv[1:]:
    show(int(a, 16))
