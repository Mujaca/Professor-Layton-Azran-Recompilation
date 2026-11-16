import os, struct
from pyctr.type.exefs import decompress_code, ExeFSEntry

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
p = os.path.join(ROOT, "out", "exefs", "code.bin")
raw = open(p, "rb").read()
dec = decompress_code(raw)
print(f"roh: {len(raw):,}  ->  dekomprimiert: {len(dec):,}  (veraendert: {dec != raw})")
if dec != raw:
    out = os.path.join(ROOT, "out", "exefs", "code_decomp.bin")
    open(out, "wb").write(dec)
    print("erste 64 Bytes:", dec[:64].hex(" "))
    print("letzte 8 Bytes (Header?):", raw[-8:].hex(" "),
          "-> comp=0x%X add=0x%X" % (struct.unpack('<I', raw[-8:-4])[0] & 0xFFFFFF,
                                     struct.unpack('<I', raw[-4:])[0]))
