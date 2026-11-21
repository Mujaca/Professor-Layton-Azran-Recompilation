"""Erzeugt aus der offiziellen Azahar-Referenz einen 400x240-RGB-Framebuffer.

Aufruf: python tools\\_make_menu_frame.py
Ergebnis: out/menu_official.bin (400*240*3 Byte, RGB)
"""
from PIL import Image

SRC = 'reports/azahar_topscreen.png'
DST = 'out/menu_official.bin'

im = Image.open(SRC).convert('RGB')
# Der Screenshot ist ein Fenster-Capture; auf die 3DS-Oberbildgroesse bringen.
im = im.resize((400, 240), Image.LANCZOS)
data = im.tobytes()
assert len(data) == 400 * 240 * 3, len(data)
with open(DST, 'wb') as f:
    f.write(data)
print('geschrieben:', DST, len(data), 'Byte')
