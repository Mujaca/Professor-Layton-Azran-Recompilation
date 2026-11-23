"""Top-Textur fuer den HLE-Render: title_a-Atlas + Ersatz-Hintergrund.

Die 3D-Titel-Map fehlt im Port; als Naeherung wird menu/title_b_bg.xi
(Spiel-Asset aus lt6_b.fa) auf 400x240 skaliert und unter den Atlas gelegt.

  python tools\\make_menu_assets.py

Erzeugt:
  out/title_top_atlas.bin   1024x1280 RGBA (Atlas + BG bei (0,1024))
  tools/profiles/title_top_rects.txt  (BG-Quad zuerst, dann 9 XPVB-Quads)
  out/title_b_bg.bin        320x240 RGB (Bottom, 1:1)
"""
import os
import struct
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc
import _atest_botscan as BS

FA = os.path.join(ROOT, 'out', 'romfs', 'lt6', 'arc', 'lt6_b.fa')
BG_OFF = 0x0CE43074
AW = 1024
AH = 1024
TW = 1024
TH = 1280


def decode_linear(path):
    """Lineare Tile-Ordnung: Kachelraster 8x8, keine Z-Order.

    Nutzt denselben Referenzdekoder wie tools/test_imgc_c.py (der
    Runtime-Dekoder in runtime/imgc.c ist damit byte-identisch).
    Rueckgabe: (w, h, rgb24-Bytes)."""
    d = open(path, 'rb').read()
    w = struct.unpack_from('<h', d, 0x10)[0]
    h = struct.unpack_from('<h', d, 0x12)[0]
    return w, h, BS.decode_linear(d, 0).tobytes()


def main():
    out = os.path.join(ROOT, 'out')
    os.makedirs(out, exist_ok=True)

    atlas = Image.open(os.path.join(ROOT, 'tools', 'profiles',
                                    'title_a_atlas.png')).convert('RGBA')
    assert atlas.size == (AW, AH)

    data = open(FA, 'rb').read()
    assert data[BG_OFF:BG_OFF + 6] == b'IMGC00', hex(BG_OFF)
    s2 = struct.unpack_from('<i', data, BG_OFF + 0x38)[0]
    sz = struct.unpack_from('<i', data, BG_OFF + 0x3C)[0]
    tmp = os.path.join(ROOT, 'tools', 'out', 'title_b_bg.xi')
    open(tmp, 'wb').write(data[BG_OFF:BG_OFF + 0x48 + s2 + sz])
    w, h, rgb = decode_linear(tmp)
    bg = Image.frombytes('RGB', (w, h), rgb)
    open(os.path.join(out, 'title_b_bg.bin'), 'wb').write(rgb)
    # 3D-Titel-Grundlayer (Host-Render aus title.xc, ETC1) bevorzugen,
    # falls vorhanden; sonst Bottom-Asset als Ersatz.
    bg3d = os.path.join(ROOT, 'tools', 'profiles', 'title3d_bg.bin')
    if os.path.exists(bg3d):
        bg = Image.frombytes('RGB', (400, 240), open(bg3d, 'rb').read())
        print('BG: 3D-Titel-Render', bg3d)

    top = Image.new('RGBA', (TW, TH), (0, 0, 0, 0))
    top.paste(atlas, (0, 0))
    top.paste(bg.convert('RGBA').resize((400, 240)), (0, AH))
    open(os.path.join(out, 'title_top_atlas.bin'), 'wb').write(top.tobytes())

    bgline = ('0 0 400 240 0 %.6f 0.390625 %.6f'
              % (AH / float(TH), (AH + 240) / float(TH)))
    lines = ['# 9 XPVB-Quads, dann der Grundlayer zuletzt (deckt Overlays)',
            ]
    for ln in open(os.path.join(ROOT, 'tools', 'profiles',
                                'title_a_rects.txt'), encoding='utf-8'):
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        x, y, qw, qh, u0, v0, u1, v1 = [float(v) for v in ln.split()[:8]]
        lines.append('%g %g %g %g %.8f %.8f %.8f %.8f'
                     % (x, y, qw, qh, u0, v0 * AH / float(TH), u1,
                        v1 * AH / float(TH)))
    lines.append(bgline)
    open(os.path.join(ROOT, 'tools', 'profiles', 'title_top_rects.txt'),
         'w', newline='\n').write('\n'.join(lines) + '\n')
    print('top-atlas %dx%d, rects %d' % (TW, TH, len(lines) - 1))


main()
