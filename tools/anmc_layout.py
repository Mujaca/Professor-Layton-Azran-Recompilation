"""ANMC00-Layout: Hash-Strings, Vorkommen, Record-Segmentierung (Analyse)."""
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'tools/out/anmc_res.bin')


def main():
    d = open(BIN, 'rb').read()
    ss = [s.decode() for s in re.findall(rb'[ -~]{3,}', d)]
    h2s = {}
    for s in ss:
        h2s[zlib.crc32(s.encode()) & 0xFFFFFFFF] = s
    print('strings=%d hashes=%d' % (len(ss), len(h2s)))
    # occurrence scan
    occ = {}
    for i in range(0, len(d) - 4):
        v = struct.unpack_from('<I', d, i)[0]
        if v in h2s and i % 1 == 0:
            occ.setdefault(v, []).append(i)
    for v in sorted(occ, key=lambda x: occ[x][0]):
        name = h2s[v]
        if name.startswith('#'):
            continue
        print('%08X %-24s @ %s' % (v, name, ' '.join('%04X' % o for o in occ[v])))
    print('\n--- command hashes')
    for v in sorted(occ, key=lambda x: occ[x][0]):
        if h2s[v].startswith('#'):
            print('%08X %-18s @ %s' % (v, h2s[v],
                                       ' '.join('%04X' % o for o in occ[v])))
    # all 4-byte words in first region as hash check
    print('\n--- unknown words 0x60..0x150')
    for i in range(0x60, 0x154, 4):
        v = struct.unpack_from('<I', d, i)[0]
        print('%04X %08X %s' % (i, v, h2s.get(v, '')))


if __name__ == '__main__':
    main()
