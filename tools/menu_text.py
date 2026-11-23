"""Menue-Text: Strings/Schrift/Lage im Capture bestimmen + Layer bauen."""

import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF = os.path.join(ROOT, 'reports/menu_official.bin')
FONTS = [r'C:\Windows\Fonts\segoeuib.ttf', r'C:\Windows\Fonts\arialbd.ttf']
ROWS = [
    ((107, 110, 105, 22), ['New Game', 'Continue', 'Options', 'Start']),
    ((84, 132, 150, 24), ['Settings', 'Options', 'Setting', 'Screen']),
]
LAYER_W, LAYER_H = 320, 80
MX, MY = 60, 12


def render_big(text, w, h, font, size):
    img = Image.new('L', (w + 2 * MX, h + 2 * MY), 0)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(font, size)
    bb = d.textbbox((0, 0), text, font=f)
    tw, th = bb[2] - bb[0], bb[3] - bb[1]
    d.text(((w + 2 * MX - tw) // 2 - bb[0], (h + 2 * MY - th) // 2 - bb[1]),
           text, font=f, fill=255)
    return np.asarray(img, dtype=np.float64)


def ncc(a, b):
    a = a - a.mean()
    b = b - b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d > 1e-9 else -9.0


def main():
    ref = np.fromfile(REF, dtype=np.uint8).reshape(240, 400, 3).astype(np.float64)
    g = ref.mean(2)
    layer = Image.new('RGBA', (LAYER_W, LAYER_H), (0, 0, 0, 0))
    lines = ['# Font-Layer fuer title_top_atlas.bin (Paste bei 400,1024)',
             '# screen_x screen_y w h layer_x layer_y']
    ly = 0
    for (x, y, w, h), cands in ROWS:
        cap = g[y - MY:y + h + MY, x - MX:x + w + MX]
        thr = cap.mean() + 0.6 * cap.std()
        binm = (cap > thr).astype(np.float64)
        best = (-9.0, '', '', 0, 0, 0, None)
        for fpath in FONTS:
            for size in range(12, 21):
                for text in cands:
                    t = render_big(text, w, h, fpath, size)
                    tmpl = t[MY:MY + h, MX:MX + w]
                    for sy in range(0, 2 * MY + 1, 2):
                        for sx in range(0, 2 * MX + 1, 2):
                            sc = ncc(tmpl, binm[sy:sy + h, sx:sx + w])
                            if sc > best[0]:
                                best = (sc, text, fpath, size,
                                        x - MX + sx, y - MY + sy, t)
        sc, text, fpath, size, px, py, t = best
        print('rect %3d,%3d %3dx%-3d -> "%-9s" %s %d  @(%3d,%3d) ncc %.3f'
              % (x, y, w, h, text, os.path.basename(fpath), size, px, py, sc))
        sub = t[MY:MY + h, MX:MX + w]
        rgba = np.zeros((h, w, 4), np.uint8)
        rgba[..., :3] = 255
        rgba[..., 3] = sub.astype(np.uint8)
        layer.paste(Image.fromarray(rgba, 'RGBA'), (0, ly))
        lines.append('%d %d %d %d %d %d' % (px, py, w, h, 0, ly))
        ly += h + 2
    layer.save(os.path.join(ROOT, 'tools/profiles/menu_text_atlas.png'))
    open(os.path.join(ROOT, 'tools/profiles/menu_text_rects.txt'),
         'w', newline='\n').write('\n'.join(lines) + '\n')
    print('layer %dx%d' % (LAYER_W, LAYER_H))


if __name__ == '__main__':
    main()
