"""ExHeader-Felder (CodeSetInfo etc.) auslesen."""
import sys, os, struct, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nokeys
nokeys.install()
from pyctr.type.cci import CCIReader

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, "Professor Layton and the Azran Legacy (Europe).3ds")

with CCIReader(ROM, assume_decrypted=True) as cci:
    ncch = cci.contents[0]
    ncch._file.seek(ncch._start + 0x200)
    exh = ncch._file.read(0x800)

print("ExHeader 0x000-0x100 (SystemInfo):")
for i in range(0, 0x100, 16):
    w = " ".join(f"{struct.unpack_from('<I', exh, i + j)[0]:08X}" for j in range(0, 16, 4))
    print(f"  {i:04X}: {w}")

# CodeSetInfo: enthaelt Text/RO/Data/BSS Adressen+Sizes und Entrypoint
print("\nKandidaten (Werte um 0x00100000 / Groessen):")
for i in range(0, 0x400 - 4, 4):
    v = struct.unpack_from('<I', exh, i)[0]
    if 0x00080000 <= v <= 0x01000000:
        print(f"  {i:04X}: 0x{v:08X}")

print("\nStrings 0x000-0x400:")
for m in re.finditer(rb'[\x20-\x7E]{4,}', exh[:0x400]):
    print(f"  {m.start():04X}: {m.group().decode('ascii')}")
