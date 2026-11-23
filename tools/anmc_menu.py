"""ANMC00-Menuelayout aus title_b.xa (lt6_uk.fa) lesen.

Der Hauptmenue-Overlay ist als ANMC00-Animation in `title_b.xa` -> `RES.bin`
gespeichert. Jeder Menuepunkt ist ein Record mit Hash-Namen
(`continue_off`, `new_on`, `sound1_off`, `try_on`, `www`, ...) und
Float-Koordinaten. Dieses Tool extrahiert die Namen + die erste plausible
(x,y)-Paarung und schreibt `tools/profiles/anmc_menu.txt`.

Damit liegen die ECHTEN Menue-Positionen aus den Spiel-Assets vor - bisher
wurden sie durch selbst erfundene Grid-Rects ersetzt.

Aufruf:  python tools/anmc_menu.py
"""
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _atest_xpck as X      # noqa: E402
import _imgc                 # noqa: E402

FA = os.path.join(ROOT, 'out/romfs/lt6/arc/lt6_uk.fa')
XA_OFF, XA_LEN = 0x01526DC0, 0x9278


def load_res():
    d = open(FA, 'rb').read()
    _cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
    e = {x['name']: x for x in ents}['RES.bin']
    raw = d[XA_OFF + e['off']:XA_OFF + e['off'] + e['size']]
    return _imgc.decomp(raw)


def menu_table(d):
    names = set(s.decode() for s in re.findall(rb'[ -~]{3,}', d))
    h2 = {zlib.crc32(s.encode()) & 0xFFFFFFFF: s for s in names}
    rows = []
    for i in range(0, len(d) - 4):
        v = struct.unpack_from('<I', d, i)[0]
        if v in h2:
            rows.append((i, h2[v]))
    out = []
    for off, name in rows:
        if not name.startswith('#'):
            continue
        fl = []
        for k in range(off, min(off + 80, len(d) - 8), 4):
            f = struct.unpack_from('<f', d, k)[0]
            if f != f or not (0.001 < abs(f) < 1000.0):
                continue
            fl.append((k - off, f))
        pair = None
        for a, b in zip(fl, fl[1:]):
            x, y = a[1], b[1]
            if 2.5 < x <= 400.0 and 2.5 < y <= 240.0 and b[0] == a[0] + 4:
                pair = (x, y)
                break
        if pair:
            out.append((off, name, pair[0], pair[1]))
    return out


def main():
    d = load_res()
    tab = menu_table(d)
    path = os.path.join(ROOT, 'tools/profiles/anmc_menu.txt')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', newline='\n') as f:
        f.write('# ANMC00-Menuelayout aus title_b.xa/RES.bin (lt6_uk.fa)\n')
        f.write('# offset name x y\n')
        for off, name, x, y in tab:
            f.write('%04X %-22s %7.2f %7.2f\n' % (off, name, x, y))
    print('records=%d -> %s' % (len(tab), path))
    seen = set()
    for _off, name, x, y in tab:
        base = name.replace('_on', '_*').replace('_off', '_*')
        if base in seen:
            continue
        seen.add(base)
        print('  %-18s (%6.1f, %6.1f)' % (name, x, y))


if __name__ == '__main__':
    main()
