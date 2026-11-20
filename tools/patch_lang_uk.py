"""Sprach-/Regions-String von "jp" auf "uk" patchen.

Die Engine baut Asset-Pfade wie "rom:/<dir>/<region>/<datei>". Der Region-
String stammt aus einer .rodata-Tabelle; fuer die japanische Auswahl zeigt
der Eintrag auf das Literal "jp" bei 0x003CBEFE. Im EUR-RomFS existiert nur
"uk" (z. B. /lt6/snd/uk/product.bcsar), daher wird das Literal auf "uk"
umgeschrieben.

Idempotent; aendert nur out/exefs/code_decomp.bin.
Danach `python tools/build_port.py` laufen lassen.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000
OFF = 0x003CBEFE

with open(PATH, 'rb') as f:
    data = bytearray(f.read())

o = OFF - BASE
old = bytes(data[o:o + 3])
if old == b'uk\x00':
    print('bereits gepatcht: %s' % old)
else:
    print('patche %s -> b"uk\\x00"' % old)
    data[o] = ord('u')
    data[o + 1] = ord('k')
    data[o + 2] = 0
    with open(PATH, 'wb') as f:
        f.write(data)
