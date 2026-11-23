"""Greedy-Auswahl der Bottom-Quads gegen den Capture (opaque, wie fmt=1)."""
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHEET = os.path.join(ROOT, 'out/anmc_bottom_sheet.bin')
RECTS = os.path.join(ROOT, 'tools/profiles/bottom_quads_real.txt')
VIS = 223


def load_rects():
    out = []
    for ln in open(RECTS, encoding='utf-8'):
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        v = [float(x) for x in ln.split()[:8]]
        out.append(v)
    return out


def blob(tex, w, h, r):
    x, y, ww, hh, u0, v0, u1, v1 = r
    x, y, ww, hh = int(round(x)), int(round(y)), int(round(ww)), int(round(hh))
    tx0 = np.clip(np.round(u0 * tex.shape[1] + (np.arange(ww) + 0.5) *
                           (u1 - u0) * tex.shape[1] / ww).astype(int), 0,
                  tex.shape[1] - 1)
    ty0 = np.clip(np.round(v0 * tex.shape[0] + (np.arange(hh) + 0.5) *
                           (v1 - v0) * tex.shape[0] / hh).astype(int), 0,
                  tex.shape[0] - 1)
    return x, y, ww, hh, tx0, ty0


def draw(canvas, tex, r):
    x, y, ww, hh, tx0, ty0 = blob(tex, 320, 512, r)
    sub = tex[np.ix_(ty0, tx0)]
    xs = np.clip(np.arange(x, x + ww), 0, 319)
    ys = np.clip(np.arange(y, y + hh), 0, 239)
    okx = (np.arange(x, x + ww) >= 0) & (np.arange(x, x + ww) < 320)
    oky = (np.arange(y, y + hh) >= 0) & (np.arange(y, y + hh) < 240)
    sub = sub[np.ix_(oky, okx)]
    canvas[np.ix_(ys[oky], xs[okx])] = sub


def metric(img, ref, rows=VIS):
    d = np.abs(img[:rows].astype(int) - ref[:rows].astype(int)).sum(axis=2)
    return int((d > 60).sum()), float(np.abs(img[:rows].astype(int) -
                                             ref[:rows].astype(int)).mean())


def main():
    tex = np.frombuffer(open(SHEET, 'rb').read(),
                        dtype=np.uint8).reshape(512, 320, 3)
    cap = np.frombuffer(open(os.path.join(
        ROOT, 'reports/menu_official_bottom.bin'), 'rb').read(),
        dtype=np.uint8).reshape(240, 320, 3)
    bg = np.frombuffer(open(os.path.join(ROOT, 'out/title_b_bg.bin'), 'rb').read(),
                       dtype=np.uint8).reshape(240, 320, 3)
    rects = load_rects()
    quads = rects[1:]
    base = bg.copy()
    print('BASE', metric(base, cap))
    # Einzelwirkung
    for i, r in enumerate(quads):
        img = bg.copy()
        draw(img, tex, r)
        print('%2d %s' % (i, metric(img, cap)))
    # Greedy: append best
    cur = bg.copy()
    used = set()
    print('\nGreedy:')
    bestm = metric(cur, cap)
    for step in range(8):
        best = None
        for i, r in enumerate(quads):
            if i in used:
                continue
            img = cur.copy()
            draw(img, tex, r)
            m = metric(img, cap)
            if best is None or (m[0], m[1]) < (best[0][0], best[0][1]):
                best = (m, i, img)
        if best is None or (best[0][0], best[0][1]) >= (bestm[0], bestm[1]):
            break
        bestm, i, img = best
        used.add(i)
        cur = img
        print(' + %2d -> %s' % (i, bestm))


if __name__ == '__main__':
    main()
