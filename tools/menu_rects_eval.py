"""Erzeugt tools/out/rects_top.txt + rects_bottom.txt + Diff-Zahlen.

Top-Atlas : tools/out/title_a_atlas.png  (1024x1024, 5 IMGC aus
            menu/uk/title_a.xa gepackt)
Top-Quads : menu/uk/title_a.xa / 000.pvb (XPVB, 36 Vertices = 9 Quads)
Bottom    : menu/title_b_bg.xi (320x240) 1:1

  python tools/menu_rects_eval.py
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc
import _atest_imgc_np as D
import _atest_xpck as X

FA = os.path.join(ROOT, 'out', 'romfs', 'lt6', 'arc', 'lt6_uk.fa')
FA_B = os.path.join(ROOT, 'out', 'romfs', 'lt6', 'arc', 'lt6_b.fa')
XA_OFF, XA_LEN = 0x01428314, 0xFEAA8
OUT = os.path.join(ROOT, 'tools', 'out')

ATLAS_POS = {'000.xi': (0, 0), '001.xi': (512, 0), '002.xi': (0, 512),
             '003.xi': (512, 512), '004.xi': (512, 768)}
AW, AH = 1024, 1024
QTEX = {0: '004.xi', 1: '002.xi', 2: '001.xi', 3: '002.xi',
        4: '004.xi', 5: '001.xi', 6: '004.xi', 7: '004.xi',
        8: '001.xi'}
PBIN = {0: '000.pbi', 1: '001.pbi', 2: '002.pbi', 3: '003.pbi',
        4: '004.pbi', 5: '004.pbi', 6: '005.pbi', 7: '005.pbi',
        8: '005.pbi'}


def load():
    d = open(FA, 'rb').read()
    cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
    ent = {e['name']: (e['off'], e['size']) for e in ents}
    tex = {}
    for e in ents:
        if e['magic'] == 'IMGC00':
            w, h, rgb, al = D.decode_buf(d, XA_OFF + e['off'])
            a = al if al is not None else np.full((h, w), 255, np.uint8)
            tex[e['name']] = (rgb, a)
    off, size = ent['000.pvb']
    pvb = d[XA_OFF + off:XA_OFF + off + size]
    vtx_off, stride, vcount = struct.unpack_from('<HHHHI', pvb, 4)[2:]
    vtx = _imgc.decomp(pvb[vtx_off:])
    vs = []
    for i in range(vcount):
        px, py, pz = struct.unpack_from('<3f', vtx, i * stride)
        u, v = struct.unpack_from('<2f', vtx, i * stride + 12)
        vs.append((px, py, pz, u, v))
    quads = [vs[q * 4:q * 4 + 4] for q in range(vcount // 4)]
    return tex, quads, d


def quad_rect(v):
    xs = [p[0] for p in v]
    ys = [p[1] for p in v]
    us = [p[3] for p in v]
    vv = [p[4] for p in v]
    return (int(round(min(xs) + 200)), int(round(min(ys) + 120)),
            int(round(max(xs) - min(xs))),
            int(round(max(ys) - min(ys))),
            int(round(min(us))), int(round(min(vv))),
            int(round(max(us))), int(round(max(vv))), v[0][2])


def draw(cv, tex, name, r):
    x, y, w, h, u0, v0, u1, v1 = [int(round(t)) for t in r[:8]]
    rgb, al = tex[name]
    th, tw = rgb.shape[:2]
    xs = np.clip(np.round(np.linspace(u0, u1, w)).astype(int), 0, tw - 1)
    ys = np.clip(np.round(np.linspace(v0, v1, h)).astype(int), 0, th - 1)
    s = rgb[np.ix_(ys, xs)].astype(np.float64)
    a = (al[np.ix_(ys, xs)] / 255.0)[:, :, None]
    sy0, sy1 = max(0, y), min(240, y + h)
    sx0, sx1 = max(0, x), min(400, x + w)
    if sy0 >= sy1 or sx0 >= sx1:
        return
    sub = s[sy0 - y:sy1 - y, sx0 - x:sx1 - x]
    sa = a[sy0 - y:sy1 - y, sx0 - x:sx1 - x]
    cv[sy0:sy1, sx0:sx1] = sub * sa + cv[sy0:sy1, sx0:sx1] * (1 - sa)


def counts(img, ref, rows, w):
    d = np.abs(img[:rows].astype(int) - ref[:rows].astype(int))
    return int((d.sum(axis=2) > 60).sum()), rows * w, float(d.mean())


def main():
    tex, quads, d = load()
    ref = np.frombuffer(open(os.path.join(
        ROOT, 'reports', 'menu_official.bin'), 'rb').read(),
        np.uint8).reshape(240, 400, 3)
    bref = np.frombuffer(open(os.path.join(
        ROOT, 'reports', 'menu_official_bottom.bin'), 'rb').read(),
        np.uint8).reshape(240, 320, 3)
    rects = [quad_rect(v) for v in quads]

    atlas = np.zeros((AH, AW, 4), np.uint8)
    for name, (ox, oy) in ATLAS_POS.items():
        rgb, al = tex[name]
        h, w = rgb.shape[:2]
        atlas[oy:oy + h, ox:ox + w, :3] = rgb
        atlas[oy:oy + h, ox:ox + w, 3] = al
    Image.fromarray(atlas, 'RGBA').save(
        os.path.join(OUT, 'title_a_atlas.png'))

    # (a) 9 echte Quads auf schwarz
    cv = np.zeros((240, 400, 3), np.float64)
    for q in sorted(range(9), key=lambda q: -rects[q][8]):
        draw(cv, tex, QTEX[q], rects[q])
    a_full = counts(cv, ref, 240, 400)
    a_vis = counts(cv, ref, 226, 400)
    print('TOP  9-Quads-Composite     diff>60 %6d/96000  sichtbar %6d/90400'
          % (a_full[0], a_vis[0]))

    # (b) Fallback: 2x2-Kachelung 003.xi + Overlay-Quads 4..8
    w3, h3, rgb3, al3 = None, None, None, None
    for e in X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)[1]:
        if e['name'] == '003.xi':
            w3, h3, rgb3, al3 = D.decode_buf(d, XA_OFF + e['off'])
    tiled = np.zeros((240, 400, 3), np.float64)
    for j in range(2):
        for i in range(2):
            x0, x1 = i * 200, (i + 1) * 200
            y0, y1 = j * 120, (j + 1) * 120
            q = rgb3[j * h3 // 2:(j + 1) * h3 // 2,
                     i * w3 // 2:(i + 1) * w3 // 2]
            tiled[y0:y1, x0:x1] = np.asarray(
                Image.fromarray(q).resize((200, 120))).astype(np.float64)
    for q in (4, 5, 6, 7, 8):
        draw(tiled, tex, QTEX[q], rects[q])
    b_full = counts(tiled, ref, 240, 400)
    b_vis = counts(tiled, ref, 226, 400)
    print('TOP  Fallback 2x2+Overlays diff>60 %6d/96000  sichtbar %6d/90400'
          % (b_full[0], b_vis[0]))

    # Bottom 1:1
    b = open(FA_B, 'rb').read()
    w, h, brgb, bal = D.decode_buf(b, 0x0CE43074)
    assert (w, h) == (320, 240)
    c_vis = counts(brgb, bref, 223, 320)
    c_full = counts(brgb, bref, 240, 320)
    print('BOTTOM title_b_bg 1:1      diff>60 %6d/76800  sichtbar %6d/71360'
          % (c_full[0], c_vis[0]))

    # ---- Dateien ----
    with open(os.path.join(OUT, 'rects_top.txt'), 'w', encoding='ascii',
              newline='\n') as f:
        f.write('# Atlas: tools/out/title_a_atlas.png %dx%d RGBA\n'
                % (AW, AH))
        f.write('#   Packliste: 000.xi@(0,0) 001.xi@(512,0) '
                '002.xi@(0,512) 003.xi@(512,512) 004.xi@(512,768)\n')
        f.write('# Quelle: menu/uk/title_a.xa 000.pvb (XPVB) '
                '@abs 0x01428424, stride 20, 36 Vertices = 9 Quads\n')
        f.write('# Format: x y w h u0 v0 u1 v1   (u/v = Atlas-Pixel)\n')
        f.write('# Screen-Mapping: x=pos_x+200, y=pos_y+120 (Mitte)\n')
        f.write('# (a) 9 echte Quads: diff>60 %d/96000, sichtbar %d/90400\n'
                % (a_full[0], a_vis[0]))
        f.write('# (b) Fallback 2x2(003.xi)+Overlays: diff>60 %d/96000, '
                'sichtbar %d/90400\n' % (b_full[0], b_vis[0]))
        f.write('# 3D-Map menu/title.xc ist nicht in den XPVB-Quads '
                'enthalten.\n')
        for q, (x, y, w, h, u0, v0, u1, v1, z) in enumerate(rects):
            ox, oy = ATLAS_POS[QTEX[q]]
            f.write('%4d %4d %4d %4d %4d %4d %4d %4d  '
                    '# quad%d %s z%.0f %s orig(%d,%d)-(%d,%d)\n'
                    % (x, y, w, h, u0 + ox, v0 + oy, u1 + ox, v1 + oy,
                       q, QTEX[q], z, PBIN[q], u0, v0, u1, v1))
        f.write('# --- Fallback 2x2-Kachelung, Atlas = 003.xi (256x256) ---\n')
        for j in range(2):
            for i in range(2):
                f.write('%4d %4d %4d %4d %4d %4d %4d %4d\n'
                        % (i * 200, j * 120, 200, 120,
                           i * 128, j * 128, (i + 1) * 128,
                           (j + 1) * 128))

    with open(os.path.join(OUT, 'rects_bottom.txt'), 'w',
              encoding='ascii', newline='\n') as f:
        f.write('# Atlas: menu/title_b_bg.xi (320x240) bzw. lt6_b.fa '
                '@0x0CE43074\n')
        f.write('# Format: x y w h u0 v0 u1 v1\n')
        f.write('# 1:1: diff>60 %d/76800 voll, %d/71360 sichtbar, '
                'mean %.2f\n' % (c_full[0], c_vis[0], c_vis[2]))
        f.write('   0    0  320  240    0    0  320  240  '
                '# title_b_bg 1:1\n')
    print('\n-> tools/out/rects_top.txt, rects_bottom.txt, '
          'title_a_atlas.png')


if __name__ == '__main__':
    main()
