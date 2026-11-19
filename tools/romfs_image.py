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

# Der Port-Parser liest die IVFC-Groessen als u32 bei 0x10/0x18/0x20 und
# die Bereiche SEQUENZIELL. Das echte Layout: Hash-Tabelle 0x208..0x1000,
# danach die RomFS-Metadaten ab 0x1000. Also L1 = 0x1000-0x208, L2 = 0,
# L3 = Groesse der Metadaten (file_data_offset aus dem Metadaten-Header).
with open(OUT, "r+b") as f:
    f.seek(0x1000)
    meta = struct.unpack("<10I", f.read(0x28))
    l3 = meta[9]                      # file_data_offset == Metadaten-Groesse
    l1 = 0x1000 - 0x208
    f.seek(0x10)
    f.write(struct.pack("<I", l1))
    f.seek(0x18)
    f.write(struct.pack("<I", 0))
    f.seek(0x20)
    f.write(struct.pack("<I", l3))
print("IVFC-Groessen gesetzt: L1=0x%X L2=0 L3=0x%X (Metadaten)" % (l1, l3))
