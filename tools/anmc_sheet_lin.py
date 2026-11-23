"""Bottom-Sheet mit LINEAR dekodiertem 000.xi neu bauen (Decode-Vergleich).

Das vorhandene Sheet (`out/anmc_bottom_sheet.bin`) nutzt den Decode aus
`_atest_imgc_np`. Hier wird derselbe Pack-Layout mit dem linearen
Tile-Decode (`tools/imgc_order.py`) gefuellt, damit messbar ist, welche
Ordnung die Textzeilen richtig abbildet.

Aufruf: python tools/anmc_sheet_lin.py [linear|z] [ziel.bin]
"""
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import anmc_pack as P     # noqa: E402
import imgc_order         # noqa: E402

XI = os.path.join(ROOT, 'tools/out/title_b_000.xi')
BG = os.path.join(ROOT, 'out/title_b_bg.bin')


def main():
    order = sys.argv[1] if len(sys.argv) > 1 else 'linear'
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        ROOT, 'out/anmc_bottom_sheet_%s.bin' % order)
    quads, _atlas = P.load_quads_and_atlas()
    uniq, seen = [], set()
    for qq in quads:
        r = (qq['u0'], qq['v0'], qq['u1'], qq['v1'])
        if r not in seen:
            seen.add(r)
            uniq.append(r)
    packed = P.shelf_pack(uniq, 320, 240, 512)
    w, h, bpp, raw = imgc_order.decode(XI, order)
    at = np.frombuffer(raw, dtype=np.uint8).reshape(h, w, bpp)
    sheet = np.zeros((512, 320, 3), np.uint8)
    bg = np.frombuffer(open(BG, 'rb').read(), np.uint8).reshape(240, 320, 3)
    sheet[:240] = bg
    for (u0, v0, u1, v1) in uniq:
        px, py = packed[(u0, v0, u1, v1)]
        crop = at[v0:v1, u0:u1, :3]
        sheet[py:py + crop.shape[0], px:px + crop.shape[1]] = crop
    open(dst, 'wb').write(sheet.tobytes())
    print('sheet %s (%dx512) -> %s  rects=%d' % (order, 320, dst, len(uniq)))


if __name__ == '__main__':
    main()
