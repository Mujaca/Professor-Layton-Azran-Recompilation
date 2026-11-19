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

# Der Parser der Engine liest die IVFC-Groessen als u32 bei 0x10/0x18/0x20
# und die Bereiche SEQUENZIELL:
#   0x208 + L1  -> Metadaten,  danach L2,  danach Metadaten (L3).
# Deshalb muessen dort die tatsaechlichen Bereichsgroessen stehen:
#   L1 = 0x1000 - 0x208 = 0xDF8, L2 = 0, L3 = 0xDD80 (Metadaten).
# Mit dem unveraenderten ROM-Header liest die Engine sonst 0 Byte
# (gemessen: off=0x208 len=0x0) und kann das RomFS nicht parsen.
with open(OUT, "r+b") as f:
    f.seek(0x10)
    f.write(struct.pack("<I", 0xDF8))
    f.seek(0x18)
    f.write(struct.pack("<I", 0))
    f.seek(0x20)
    f.write(struct.pack("<I", 0xDD80))
print("IVFC-Groessen fuer die Engine gesetzt: L1=0xDF8 L2=0 L3=0xDD80")

