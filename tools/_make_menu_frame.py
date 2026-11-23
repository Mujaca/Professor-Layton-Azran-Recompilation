"""Erzeugt aus den offiziellen Azahar-Referenzen die 3DS-Screen-Frames.

Aufruf: python tools\\_make_menu_frame.py
Ergebnis:
  out/menu_official.bin        400x240 RGB (Oberschirm)
  reports/menu_official_bottom.bin  320x240 RGB (Unterschirm)
  out/menu_official_bottom.bin       (Kopie fuer den Port)

Der Azahar-Capture ist ein Fensterfoto:
  Oben : 432x239, Fensterleiste y=0..12, aktiver Screen x=16..415,
         y=13..238 -> 226 sichtbare Zeilen (14 fehlen unten)
  Unten: 432x223, aktiver Screen x=56..375 (320 px), 17 Zeilen fehlen -> schwarz
"""
import os
from PIL import Image

SRC = 'reports/azahar_topscreen.png'
DST = 'out/menu_official.bin'

im = Image.open(SRC).convert('RGB')
# Frueher wurde das ganze Fenster (inkl. Bezel) auf 400x240 skaliert. Das
# hat den Screen horizontal gestaucht und links/rechts schwarze Raender
# eingebaut. Jetzt exakt den Screenbereich ausschneiden und die im Capture
# fehlenden Zeilen schwarz auffuellen (wie beim Unterschirm).
crop = im.crop((16, 13, 416, 239))          # 400 x 226
canvas = Image.new('RGB', (400, 240), (0, 0, 0))
canvas.paste(crop, (0, 0))
data = canvas.tobytes()
assert len(data) == 400 * 240 * 3, len(data)
for dst in (DST, 'reports/menu_official.bin'):
    with open(dst, 'wb') as f:
        f.write(data)
    print('geschrieben:', dst, len(data), 'Byte (sichtbar: %dx%d)' % crop.size)

# --- Unterschirm (320x240) ---
BSRC = 'reports/azahar_bottomscreen.png'
BDST_REPORTS = 'reports/menu_official_bottom.bin'
BDST_OUT = 'out/menu_official_bottom.bin'
bim = Image.open(BSRC).convert('RGB')
crop = bim.crop((56, 0, 56 + 320, bim.size[1]))   # 320 x 223
canvas = Image.new('RGB', (320, 240), (0, 0, 0))
canvas.paste(crop, (0, 0))
bdata = canvas.tobytes()
assert len(bdata) == 320 * 240 * 3, len(bdata)
for dst in (BDST_REPORTS, BDST_OUT):
    with open(dst, 'wb') as f:
        f.write(bdata)
    print('geschrieben:', dst, len(bdata), 'Byte')
