"""Dump ANMC-Records: je Hash-Vorkommen die folgenden 16 Woerter (hex+float)."""
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'tools/out/anmc_res.bin')
OUT = os.path.join(ROOT, 'tools/out/anmc_records.txt')


def f32(v):
    return struct.unpack('<f', struct.pack('<I', v))[0]


def main():
    d = open(BIN, 'rb').read()
    ss = [s.decode() for s in re.findall(rb'[ -~]{3,}', d)]
    h2s = {}
    for s in ss:
        h2s[zlib.crc32(s.encode()) & 0xFFFFFFFF] = s
    starts = []
    for i in range(0, len(d) - 4, 4):
        v = struct.unpack_from('<I', d, i)[0]
        if v in h2s:
            starts.append((i, h2s[v]))
    lines = []
    for i, name in starts:
        words = []
        for k in range(16):
            if i + k * 4 + 4 <= len(d):
                v = struct.unpack_from('<I', d, i + k * 4)[0]
                words.append('%08X' % v)
            else:
                words.append('--------')
        fl = []
        for k in range(1, 16):
            if i + k * 4 + 4 <= len(d):
                fl.append('%g' % f32(struct.unpack_from('<I', d, i + k * 4)[0]))
        lines.append('%04X %-22s | %s' % (i, name, ' '.join(words)))
        lines.append('     %-22s | %s' % ('', ' '.join(fl)))
    txt = '\n'.join(lines)
    open(OUT, 'w').write(txt + '\n')
    print(txt)


if __name__ == '__main__':
    main()
