"""ANMC00 (title_b.xa RES.bin) Struktur-Probe. Reines Analyse-Skript."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), 'tools'))
import _atest_xpck as X
import _imgc

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FA = os.path.join(ROOT, 'out/romfs/lt6/arc/lt6_uk.fa')
XA_OFF, XA_LEN = 0x01526DC0, 0x9278


def main():
    d = open(FA, 'rb').read()
    cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
    for e in ents:
        mg = ''.join(c if 32 <= ord(c) < 127 else '.' for c in e['magic'])
        print('%-12s off=%06X size=%6d magic=%s'
              % (e['name'], e['off'], e['size'], mg))
    ent = {e['name']: e for e in ents}
    e = ent['RES.bin']
    raw = d[XA_OFF + e['off']:XA_OFF + e['off'] + e['size']]
    print('\nRES.bin raw %d bytes, magic=%r' % (len(raw), raw[:6]))
    dec = _imgc.decomp(raw)
    print('decompressed %d bytes' % len(dec))
    open(os.path.join(ROOT, 'tools/out/anmc_res.bin'), 'wb').write(dec)
    for i in range(0, min(len(dec), 256), 16):
        print('%04X  %s' % (i, ' '.join('%02X' % b for b in dec[i:i + 16])))
    print()
    # ASCII strings
    ss = []
    cur = bytearray()
    for b in dec:
        if 32 <= b < 127:
            cur.append(b)
        else:
            if len(cur) >= 4:
                ss.append(bytes(cur))
            cur = bytearray()
    print('strings:', [s.decode() for s in ss])


if __name__ == '__main__':
    main()
