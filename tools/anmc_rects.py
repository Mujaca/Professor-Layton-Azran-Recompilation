"""ANMC-Menuepunkte als Sprite-Rects exportieren.

Bildet die in `tools/profiles/anmc_menu.txt` extrahierten ANMC-Positionen
mit der in `tools/anmc_fit.py` gemessenen Abbildung auf den Top-Screen ab
(sx=1.25, y von unten, dx=24) und schreibt je Menuepunkt ein Quad.

Textur: 000.xi (Overlay-Atlas, 256x512) - die Glyph-Baender liegen bei
v=160..200. Die UVs zeigen auf diesen Bereich; Groesse 96x16 ist die
Breite eines Menue-Eintrags in der Referenz.

Aufruf: python tools/anmc_rects.py
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MENU = os.path.join(ROOT, 'tools/profiles/anmc_menu.txt')
OUT = os.path.join(ROOT, 'tools/profiles/anmc_rects.txt')

SX, DX = 1.25, 24.0
W, H = 96, 16
V0, V1 = 160.0 / 512.0, 200.0 / 512.0


def main():
    seen = set()
    rows = []
    for line in open(MENU, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        parts = line.split()
        name = ' '.join(parts[1:-2])
        x, y = float(parts[-2]), float(parts[-1])
        if name.startswith('#CHP'):
            continue
        base = name.replace('_off', '_*').replace('_on', '_*')
        if base in seen:
            continue
        seen.add(base)
        X = x * SX + DX
        Y = 240.0 - y * SX
        rows.append((name, int(round(X - W / 2)), int(round(Y - H / 2))))
    with open(OUT, 'w', newline='\n') as f:
        f.write('# ANMC-Menuepunkte (title_b.xa), sx=1.25 dx=24, y von unten\n')
        f.write('# x y w h u0 v0 u1 v1   (Textur 000.xi, Glyph-Band v=160..200)\n')
        for name, x, y in rows:
            f.write('%d %d %d %d %.6f %.6f 1.000000 %.6f\n'
                    % (x, y, W, H, 0.0, V0, V1))
    print('elements=%d -> %s' % (len(rows), OUT))
    for name, x, y in rows:
        print('  %-20s (%3d,%3d)' % (name, x, y))


if __name__ == '__main__':
    main()
