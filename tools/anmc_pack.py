"""Paket 4: ANMC-Platzierung der 23 title_b.xa-Quads -> Bottom-Rectliste.

Baut:
  out/anmc_bottom_sheet.bin   320x512 RGB: BG (Zeilen 0..239) + gepackte
                              000.xi-UV-Rects (Zeilen 240..511)
  out/anmc_top_atlas.bin      1024x1664 RGBA: title_a @(0,376), BG @(0,1400),
                              Bottom-Sheet-Bytes ab Byte 0x100000
  tools/profiles/bottom_quads_real.txt   1 BG-Quad + 23 echte Quads
  tools/profiles/anmc_top_rects.txt      Top-Rects mit v-Offset 376

Die ANMC-Platzierung wird aus RES.bin (dekomprimiert) gelesen:
Record = hash, u32, float[10]; Origin = (f1+f4, f2+f5), Groesse =
(f7-f4, f8-f5). Quad-Position = Origin + XPVB-lokalem (lx,ly).
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _atest_xpck as X
import _atest_imgc_np as D
import _imgc

FA = os.path.join(ROOT, 'out/romfs/lt6/arc/lt6_uk.fa')
XA_OFF, XA_LEN = 0x01526DC0, 0x9278
ANMC = os.path.join(ROOT, 'tools/out/anmc_res.bin')

SHEET_W, SHEET_H = 320, 512
TOP_W, TOP_H = 1024, 1664
TITLE_A_ROW = 376
BG_ROW = 1400


def f32(v):
    return struct.unpack('<f', struct.pack('<I', v))[0]


def anmc_rec(off):
    d = open(ANMC, 'rb').read()
    f = [f32(struct.unpack_from('<I', d, off + 8 + 4 * k)[0])
         for k in range(10)]
    return dict(f1=f[1], f2=f[2], f4=f[4], f5=f[5], f7=f[7], f8=f[8],
                w=f[7] - f[4], h=f[8] - f[5])


def load_quads_and_atlas():
    d = open(FA, 'rb').read()
    cnt, ents = X.parse(d, XA_OFF, XA_LEN, _imgc.decomp)
    ent = {e['name']: e for e in ents}
    pvb = d[XA_OFF + ent['000.pvb']['off']:
            XA_OFF + ent['000.pvb']['off'] + ent['000.pvb']['size']]
    vtx_off, stride, vcount = struct.unpack_from('<HHHHI', pvb, 4)[2:]
    vtx = _imgc.decomp(pvb[vtx_off:])
    vs = []
    for i in range(vcount):
        px, py, pz = struct.unpack_from('<3f', vtx, i * stride)
        u, v = struct.unpack_from('<2f', vtx, i * stride + 12)
        vs.append((px, py, pz, u, v))
    quads = []
    for q in range(vcount // 4):
        v = vs[q * 4:q * 4 + 4]
        x0 = min(p[0] for p in v)
        y0 = min(p[1] for p in v)
        x1 = max(p[0] for p in v)
        y1 = max(p[1] for p in v)
        quads.append(dict(q=q, lx=x0, ly=y0, w=x1 - x0, h=y1 - y0,
                          u0=int(round(min(p[3] for p in v))),
                          v0=int(round(min(p[4] for p in v))),
                          u1=int(round(max(p[3] for p in v))),
                          v1=int(round(max(p[4] for p in v)))))
    e = ent['000.xi']
    o = XA_OFF + e['off']
    w, h, rgb, a = D.decode_buf(d, o)
    if a is None:
        a = np.full((h, w), 255, np.uint8)
    rgba = np.empty((h, w, 4), np.uint8)
    rgba[..., :3] = rgb
    rgba[..., 3] = a
    return quads, rgba


# Quad -> ANMC-Basisrecord (Anker/Idle-Keyframe) und Name.
# Basis = "off"/"on"-Record mit BBox f4/f5 = lokalem Quad-Ursprung.
PLACE = {
    0: (0xCAC, 'continue_off', False),
    1: (0xEBC, 'new_off', False),
    2: (0xD6C, 'scr_off', False),
    3: (0xCAC, 'continue_off', False),
    4: (0xCAC, 'continue_off', False),
    5: (0xEBC, 'new_off', False),
    6: (0xEBC, 'new_off', False),
    7: (0xDCC, 'pointer', True),
    8: (0xD6C, 'scr_off', False),
    9: (0xD6C, 'scr_off', False),
    10: (0xE5C, 'setting_off', False),
    11: (0xE5C, 'setting_off', False),
    12: (0xF4C, 'sound1_on', False),
    13: (0xF4C, 'sound1_on', False),
    14: (0xF4C, 'sound1_on', False),
    15: (0xFAC, 'sound2_on', False),
    16: (0xFAC, 'sound2_on', False),
    17: (0x100C, 'sound3_on', False),
    18: (0x100C, 'sound3_on', False),
    19: (0x103C, 'try_off', False),
    20: (0x103C, 'try_off', False),
    21: (0x103C, 'try_off', False),
    22: (0x109C, 'try_www', False),
}


def quad_placement(q, rec, special):
    if special:  # Pointer: VBO-Koordinaten liegen bereits in Node-Lokalraum
        return int(round(rec['f1'] + q['lx'])), int(round(rec['f2'] + q['ly']))
    return (int(round(rec['f1'] + rec['f4'] + q['lx'])),
            int(round(rec['f2'] + rec['f5'] + q['ly'])))


def shelf_pack(rects, width, y0, y1, gap=1):
    """rects: Liste (u0,v0,u1,v1,w,h). Rueckgabe: dict rect->(px,py)."""
    boxes = []
    for r in rects:
        boxes.append((r[2] - r[0], r[3] - r[1], r))
    boxes.sort(key=lambda b: (-b[1], -b[0]))
    out = {}
    x, y, rowh = 0, y0, 0
    for (w, h, r) in boxes:
        if w > width:
            raise SystemExit('rect zu breit: %s' % (r,))
        if x + w > width:
            x = 0
            y += rowh + gap
            rowh = 0
        if y + h > y1:
            raise SystemExit('Atlas voll bei %s' % (r,))
        out[r] = (x, y)
        x += w + gap
        rowh = max(rowh, h)
    return out


def main():
    quads, atlas = load_quads_and_atlas()
    bg = np.frombuffer(open(os.path.join(ROOT, 'out/title_b_bg.bin'), 'rb').read(),
                       dtype=np.uint8).reshape(240, 320, 3)

    # eindeutige UV-Rects sammeln
    uniq = []
    seen = set()
    for q in quads:
        r = (q['u0'], q['v0'], q['u1'], q['v1'])
        if r not in seen:
            seen.add(r)
            uniq.append(r)
    packed = shelf_pack(uniq, SHEET_W, 240, SHEET_H)

    sheet = np.zeros((SHEET_H, SHEET_W, 3), np.uint8)
    sheet[0:240] = bg
    for r, (px, py) in packed.items():
        u0, v0, u1, v1 = r
        sheet[py:py + (v1 - v0), px:px + (u1 - u0)] = atlas[v0:v1, u0:u1, :3]
    open(os.path.join(ROOT, 'out/anmc_bottom_sheet.bin'), 'wb').write(
        sheet.tobytes())

    # Rectliste: BG zuerst, dann 23 Quads in Malreihenfolge
    lines = ['# Bottom aus echten title_b.xa-Quads (ANMC-Platzierung, 320x240).',
             '# Sheet: out/anmc_bottom_sheet.bin 320x512 RGB.',
             '# BG-Zeilen 0..239, 000.xi-UVs gepackt ab Zeile 240.',
             '# Format: x y w h u0 v0 u1 v1 (UV in 0..1)',
             '0 0 320 240 0 0.00000000 1.00000000 0.46875000']
    order = [20, 21, 19, 22, 3, 8, 10, 5, 0, 2, 12, 13, 14, 15, 16, 17, 18,
             1, 4, 6, 9, 11, 7]
    for qi in order:
        q = quads[qi]
        off, name, special = PLACE[qi]
        rec = anmc_rec(off)
        x, y = quad_placement(q, rec, special)
        px, py = packed[(q['u0'], q['v0'], q['u1'], q['v1'])]
        w, h = int(round(q['w'])), int(round(q['h']))
        u0 = px / float(SHEET_W)
        v0 = py / float(SHEET_H)
        u1 = (px + w) / float(SHEET_W)
        v1 = (py + h) / float(SHEET_H)
        lines.append('%d %d %d %d %.8f %.8f %.8f %.8f'
                     % (x, y, w, h, u0, v0, u1, v1))
        print('quad %2d -> %-22s @(%3d,%3d) %dx%d uv=(%.3f,%.3f)-(%.3f,%.3f)'
              % (qi, name, x, y, w, h, u0, v0, u1, v1))
    open(os.path.join(ROOT, 'tools/profiles/bottom_quads_real.txt'), 'w',
         newline='\n').write('\n'.join(lines) + '\n')

    # Top-Atlas: title_a @376, BG @1400, Bottom-Sheet-Bytes ab 0x100000
    top = Image.new('RGBA', (TOP_W, TOP_H), (0, 0, 0, 0))
    atlas_a = Image.open(os.path.join(ROOT, 'tools/profiles/title_a_atlas.png')
                         ).convert('RGBA')
    top.paste(atlas_a, (0, TITLE_A_ROW))
    bg3d = os.path.join(ROOT, 'tools/profiles/title3d_bg.bin')
    if os.path.exists(bg3d):
        topbg = Image.frombytes('RGB', (400, 240),
                                open(bg3d, 'rb').read()).convert('RGBA')
    else:
        topbg = Image.frombytes('RGB', (320, 240),
                                open(os.path.join(ROOT, 'out/title_b_bg.bin'),
                                     'rb').read()).convert('RGBA')
        topbg = topbg.resize((400, 240))
    top.paste(topbg, (0, BG_ROW))
    raw = bytearray(top.tobytes())
    sheetbytes = sheet.tobytes()
    assert len(sheetbytes) == 120 * TOP_W * 4
    off = TITLE_A_ROW * TOP_W * 4
    raw[off:off + len(sheetbytes)] = sheetbytes
    open(os.path.join(ROOT, 'out/anmc_top_atlas.bin'), 'wb').write(bytes(raw))

    # Top-Rects: title_top_rects.txt mit v um TITLE_A_ROW verschieben
    out = ['# Top-Rects fuer anmc_top_atlas.bin (1024x1664), v-Offset %d'
           % TITLE_A_ROW]
    for ln in open(os.path.join(ROOT, 'tools/profiles/title_top_rects.txt'),
                   encoding='utf-8'):
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        x, y, w, h, u0, v0, u1, v1 = [float(v) for v in ln.split()[:8]]
        v0p = (v0 * 1280.0 + TITLE_A_ROW) / TOP_H
        v1p = (v1 * 1280.0 + TITLE_A_ROW) / TOP_H
        out.append('%g %g %g %g %.8f %.8f %.8f %.8f'
                   % (x, y, w, h, u0, v0p, u1, v1p))
    open(os.path.join(ROOT, 'tools/profiles/anmc_top_rects.txt'), 'w',
         newline='\n').write('\n'.join(out) + '\n')
    print('written out/anmc_bottom_sheet.bin, out/anmc_top_atlas.bin, '
          'profiles/bottom_quads_real.txt, profiles/anmc_top_rects.txt')


if __name__ == '__main__':
    main()
