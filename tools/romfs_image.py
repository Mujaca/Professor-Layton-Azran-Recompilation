"""Extrahiert das RomFS-Image (SelfNCCH) aus dem 3DS-ROM.

Der Port braucht es, weil das Spiel sein eigenes RomFS ueber
fs:USER OpenFileDirectly(Archive-ID 3 = SelfNCCH) oeffnet und dann
den IVFC-Header sowie die Hash-/Metadatentabellen liest.
"""

import struct
import sys

ROM = sys.argv[1] if len(sys.argv) > 1 else \
    r"Professor Layton and the Azran Legacy (Europe).3ds"
OUT = sys.argv[2] if len(sys.argv) > 2 else r"out\romfs_image.bin"

with open(ROM, "rb") as f:
    head = f.read(0x200)

if head[0x100:0x104] != b"NCSD":
    raise SystemExit("kein NCSD-Header")

part_off = struct.unpack_from("<I", head, 0x120)[0]   # Partition 1 (App)
ncch_off = part_off * 0x200

with open(ROM, "rb") as f:
    f.seek(ncch_off + 0x100)
    magic = f.read(4)
    f.seek(ncch_off + 0x1B0)
    romfs_off, romfs_size = struct.unpack("<II", f.read(8))

if magic != b"NCCH":
    raise SystemExit("kein NCCH bei 0x%X" % ncch_off)

start = ncch_off + romfs_off * 0x200
size = romfs_size * 0x200
print("NCCH @0x%X  RomFS @0x%X  %d Byte" % (ncch_off, start, size))

with open(ROM, "rb") as f, open(OUT, "wb") as g:
    f.seek(start)
    left = size
    while left > 0:
        chunk = f.read(min(1 << 20, left))
        if not chunk:
            break
        g.write(chunk)
        left -= len(chunk)

print("geschrieben:", OUT)

# Hinweis: Der IVFC-Header bleibt UNVERAENDERT (Original aus dem ROM).
# Frueher wurden hier die Groessen bei 0x10/0x18/0x20 ueberschrieben; das
# hat den Mount der Engine nicht verbessert (fn_0012BC58 liefert weiter 0),
# daher ist der originale Header die ehrlichere Variante.

