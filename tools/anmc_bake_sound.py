"""Baked-Alpha-Variante: nur Sound-Quads + Pointer (fmt=1 kann kein Alpha).

Schreibt out/anmc_bottom_sheet_sound.bin + profiles/bottom_quads_sound.txt
und tools/profiles/bottom_quads_sound.json.
"""
import json
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import anmc_pack as P
import anmc_alpha_search as A

SEL = [15, 18, 7]


def main():
    quads, atlas = P.load_quads_and_atlas()
    bg = np.frombuffer(open(os.path.join(ROOT, 'out/title_b_bg.bin'), 'rb').read(),
                       dtype=np.uint8).reshape(240, 320, 3)
    tex = np.frombuffer(open(os.path.join(ROOT, 'out/anmc_bottom_sheet.bin'),
                             'rb').read(), dtype=np.uint8).reshape(512, 320, 3)
    tex = tex.copy()

    # gleiche Pack-Positionen wie anmc_pack
    uniq, seen = [], set()
    for qq in quads:
        r = (qq['u0'], qq['v0'], qq['u1'], qq['v1'])
        if r not in seen:
            seen.add(r)
            uniq.append(r)
    packed = P.shelf_pack(uniq, P.SHEET_W, 240, P.SHEET_H)

    pos = {}
    for qi in range(23):
        off, name, sp = P.PLACE[qi]
        pos[qi] = P.quad_placement(quads[qi], P.anmc_rec(off), sp) + (name,)

    canvas = bg.astype(np.float32)
    lines = ['# Baked-Alpha: BG + Sound-Icons + Pointer (echte ANMC-Positionen).',
             '# Format: x y w h u0 v0 u1 v1',
             '0 0 320 240 0 0.00000000 1.00000000 0.46875000']
    for qi in SEL:
        q = quads[qi]
        x, y, name = pos[qi]
        px, py = packed[(q['u0'], q['v0'], q['u1'], q['v1'])]
        dstw, dsth = int(round(q['w'])), int(round(q['h']))
        tx = np.clip(np.round(q['u0'] + (np.arange(dstw) + 0.5) *
                              (q['u1'] - q['u0']) / dstw).astype(int),
                     0, atlas.shape[1] - 1)
        ty = np.clip(np.round(q['v0'] + (np.arange(dsth) + 0.5) *
                              (q['v1'] - q['v0']) / dsth).astype(int),
                     0, atlas.shape[0] - 1)
        src = atlas[np.ix_(ty, tx)].astype(np.float32)
        a = src[..., 3:4] / 255.0
        xs = np.arange(x, x + dstw)
        ys = np.arange(y, y + dsth)
        okx = (xs >= 0) & (xs < 320)
        oky = (ys >= 0) & (ys < 240)
        cur = canvas[np.ix_(ys[oky], xs[okx])]
        srcs = src[np.ix_(oky, okx)]
        out = srcs[..., :3] * a[np.ix_(oky, okx)] + \
            cur * (1.0 - a[np.ix_(oky, okx)])
        out8 = np.clip(out, 0, 255).astype(np.uint8)
        tex[np.ix_(np.arange(py, py + dsth)[oky], np.arange(px, px + dstw)[okx])] = out8
        canvas[np.ix_(ys[oky], xs[okx])] = out
        lines.append('%d %d %d %d %.8f %.8f %.8f %.8f'
                     % (x, y, dstw, dsth, px / 320.0, py / 512.0,
                        (px + dstw) / 320.0, (py + dsth) / 512.0))
        print('baked', qi, name, (x, y), metric_str(canvas, bg))
    open(os.path.join(ROOT, 'out/anmc_bottom_sheet_sound.bin'), 'wb').write(
        tex.tobytes())
    open(os.path.join(ROOT, 'tools/profiles/bottom_quads_sound.txt'), 'w',
         newline='\n').write('\n'.join(lines) + '\n')
    prof = json.load(open(os.path.join(ROOT, 'tools/profiles/bottom_quads.json'),
                          encoding='utf-8'))
    prof['env']['HLE_MENU_TEX_BOT'] = 'out/anmc_bottom_sheet_sound.bin'
    prof['env']['HLE_PICA_SPRITES2'] = 'tools/profiles/bottom_quads_sound.txt'
    prof['_comment'] = ('Baked-Alpha-Diagnose: BG + Sound-Icons + Pointer. '
                        'Nur diese echten Quads senken diff>60 UND mean.')
    json.dump(prof, open(os.path.join(ROOT, 'tools/profiles/bottom_quads_sound.json'),
                         'w', encoding='utf-8'), indent=1)
    print('written out/anmc_bottom_sheet_sound.bin, '
          'profiles/bottom_quads_sound.txt/json')


def metric_str(canvas, bg):
    cap = np.frombuffer(open(os.path.join(
        ROOT, 'reports/menu_official_bottom.bin'), 'rb').read(),
        dtype=np.uint8).reshape(240, 320, 3)
    return A.metric(np.clip(canvas, 0, 255).astype(np.uint8), cap)


if __name__ == '__main__':
    main()
