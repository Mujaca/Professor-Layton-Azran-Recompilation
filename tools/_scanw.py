"""Scannt code_decomp.bin nach 32-Bit-Woertern und gibt Fundstellen aus."""
import sys
import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000

data = open(PATH, 'rb').read()
vals = [int(x, 0) for x in sys.argv[1:]]
for v in vals:
    hits = []
    pat = struct.pack('<I', v)
    off = data.find(pat)
    while off >= 0:
        hits.append(off + BASE)
        off = data.find(pat, off + 1)
    print('0x%08X: %d Treffer' % (v, len(hits)))
    for h in hits[:40]:
        print('  0x%08X' % h)
    if len(hits) > 40:
        print('  ...')
