"""Menue-Elemente: ANMC-Namen mit XPVB-Quads (Groesse + UV) paaren.

Beleg: die ANMC-Floats sind (Breite, Hoehe) eines Menuepunkts, keine
Position. Das zeigt der Vergleich mit `title_b.xa/000.pvb`: jede Groesse
stimmt exakt mit einem XPVB-Quad ueberein (103x22, 105x22, 99x22,
150x24, 24x26, 256x64, 144x24).

Ausgabe `tools/profiles/menu_elements.txt`:
  name w h u0off v0off u1off v1off u0on v0on u1on v1on

Die Bildschirmposition steht NICHT hier - sie entsteht aus den
#CHP-Transforms der ANMC (offen, siehe docs/PLAN_MENU_REAL.md).

Aufruf: python tools/menu_elements.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MENU = os.path.join(ROOT, 'tools/profiles/anmc_menu.txt')
RECTS = os.path.join(ROOT, 'tools/out/titleb_rects.txt')
OUT = os.path.join(ROOT, 'tools/profiles/menu_elements.txt')

PAT = re.compile(r'quad\s+(\d+)\s+pos=\(\s*(-?[\d.]+),\s*(-?[\d.]+)\)'
                 r'\s+([\d.]+)x([\d.]+)\s+uv=\(\s*(\d+),\s*(\d+)\)-'
                 r'\(\s*(\d+),\s*(\d+)\)')

# (ANMC-Name, off-Quad, on-Quad) - Zuordnung ueber die gemessene Groesse.
MAP = [
    ('continue_off', 0, 3), ('continue_on', 0, 3),
    ('new_off', 1, 5), ('new_on', 1, 5),
    ('scr_off', 2, 8), ('scr_on', 2, 8),
    ('setting_off', 10, 11), ('setting_on', 10, 11),
    ('sound1_off', 12, 13), ('sound1_on', 12, 13),
    ('sound2_off', 14, 15), ('sound2_on', 14, 15),
    ('sound3_off', 16, 17), ('sound3_on', 16, 17),
    ('try_off', 19, 20), ('try_on', 19, 20),
    ('www', 22, 22), ('pointer', 7, 7),
]


def pvb():
    rows = {}
    for line in open(RECTS, encoding='utf-16'):
        m = PAT.search(line)
        if m:
            rows[int(m.group(1))] = (float(m.group(4)), float(m.group(5)),
                                     int(m.group(6)), int(m.group(7)),
                                     int(m.group(8)), int(m.group(9)))
    return rows


def anmc():
    tab = {}
    for line in open(MENU, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        parts = line.split()
        name = ' '.join(parts[1:-2])
        name = re.sub(r'\s+\d+$', '', name).lstrip('#')
        tab.setdefault(name, (float(parts[-2]), float(parts[-1])))
    return tab


def main():
    q = pvb()
    a = anmc()
    out = []
    bad = 0
    for name, qo, qon in MAP:
        if name not in a:
            print('  ! %-14s fehlt in ANMC' % name)
            bad += 1
            continue
        w, h = a[name]
        wo, ho, u0, v0, u1, v1 = q[qo]
        wn, hn, u0n, v0n, u1n, v1n = q[qon]
        ok = abs(w - wo) <= 2.0 and abs(w - wn) <= 2.0
        if not ok:
            bad += 1
        print('  %-14s ANMC %5.0fx%-4.0f  XPVB %5.0fx%-4.0f  off uv %d,%d-%d,%d  on uv %d,%d-%d,%d  %s'
              % (name, w, h, wo, ho, u0, v0, u1, v1, u0n, v0n, u1n, v1n,
                 'OK' if ok else 'MISMATCH'))
        out.append((name, w, h, u0, v0, u1, v1, u0n, v0n, u1n, v1n))
    with open(OUT, 'w', newline='\n') as f:
        f.write('# Menue-Elemente: ANMC-Name + XPVB-Groesse/UV (title_b.xa)\n')
        f.write('# name w h u0off v0off u1off v1off u0on v0on u1on v1on\n')
        for r in out:
            f.write('%s %.0f %.0f %d %d %d %d %d %d %d %d\n' % r)
    print('elements=%d -> %s (Mismatches: %d)' % (len(out), OUT, bad))


if __name__ == '__main__':
    main()
