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
    # Registriertes Spiel-Asset (title_b_bg, 400x240) hat Vorrang: es trifft
    # den Capture deutlich besser (corr 0.435) als der ETC1-Host-Render
    # (corr 0.095). Beleg: tools/bg_fit_bottom.py, docs/PORT_STATE.md.
    bgv2 = os.path.join(ROOT, 'tools', 'profiles', 'title_bg_v2.bin')
    bgreg = os.path.join(ROOT, 'tools', 'profiles', 'title_bg_registered.bin')
    bg3d = os.path.join(ROOT, 'tools', 'profiles', 'title3d_bg.bin')
    if os.environ.get('HLE_BG_V2') == '1' and os.path.exists(bgv2):
        bg = Image.frombytes('RGB', (400, 240), open(bgv2, 'rb').read())
        print('BG: v2 (3D-Render + Asset, corr 0.455, std 16.3)', bgv2)
    elif os.path.exists(bgreg):
        bg = Image.frombytes('RGB', (400, 240), open(bgreg, 'rb').read())
        print('BG: registriertes Spiel-Asset', bgreg)
    elif os.path.exists(bg3d):
        bg = Image.frombytes('RGB', (400, 240), open(bg3d, 'rb').read())
        print('BG: 3D-Titel-Render', bg3d)

    top = Image.new('RGBA', (TW, TH), (0, 0, 0, 0))
    top.paste(atlas, (0, 0))
    top.paste(bg.convert('RGBA').resize((400, 240)), (0, AH))
    # Font-Layer (aus tools/menu_text.py) in den freien Bereich rechts
    # neben dem BG legen: 400..1023 x 1024..1263.
    TX, TY = 400, AH
    text_lines = []
    tpath = os.path.join(ROOT, 'tools', 'profiles', 'menu_text_atlas.png')
    rpath = os.path.join(ROOT, 'tools', 'profiles', 'menu_text_rects.txt')
    if os.path.exists(tpath) and os.path.exists(rpath):
        timg = Image.open(tpath).convert('RGBA')
        top.alpha_composite(timg, (TX, TY))
        for ln in open(rpath, encoding='utf-8'):
            ln = ln.strip()
            if not ln or ln.startswith('#'):
                continue
            sx, sy, w, h, lx, ly = [int(v) for v in ln.split()[:6]]
            text_lines.append('%d %d %d %d %.8f %.8f %.8f %.8f'
                              % (sx, sy, w, h,
                                 (TX + lx) / float(TW),
                                 (TY + ly) / float(TH),
                                 (TX + lx + w) / float(TW),
                                 (TY + ly + h) / float(TH)))
        print('Font-Layer %dx%d bei (%d,%d), %d Zeilen'
              % (timg.size[0], timg.size[1], TX, TY, len(text_lines)))
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
    lines.extend(text_lines)
    open(os.path.join(ROOT, 'tools', 'profiles', 'title_top_rects.txt'),
         'w', newline='\n').write('\n'.join(lines) + '\n')
    print('top-atlas %dx%d, rects %d' % (TW, TH, len(lines) - 1))


main()
