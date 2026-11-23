"""title_a-Atlas (5 IMGC aus menu/uk/title_a.xa) als RGB-Rohdatei schreiben.

  python tools\\make_titlea_atlas.py

Quelle: tools/profiles/title_a_atlas.png (1024x1024 RGBA, aus
menu/uk/title_a.xa 000.xi..004.xi gepackt).
Ziel:   tools/out/title_a_atlas.bin (1024x1024 RGB, fuer HLE_MENU_TEX).
"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(ROOT, 'tools', 'profiles', 'title_a_atlas.png')
dst = os.path.join(ROOT, 'tools', 'out', 'title_a_atlas.bin')
im = Image.open(src).convert('RGB')
os.makedirs(os.path.dirname(dst), exist_ok=True)
open(dst, 'wb').write(im.tobytes())
print('%s %s -> %s' % (src, im.size, dst))
