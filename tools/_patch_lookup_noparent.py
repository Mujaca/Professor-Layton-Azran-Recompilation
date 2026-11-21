"""RomFS-Lookup seed-unabhaengig machen (nur Namensvergleich).

Kontext: fn_00212E3C hasht pro Pfadkomponente und prueft dann Parent-Offset
und Laenge. Im Port stimmt der Parent-/Seed-Pfad fuer die .fa-Dateien nicht,
obwohl die Records (und mit _romfs_fullchain.py eine vollstaendige Kette)
vorhanden sind.

Patch:
  0x212F3C  cmp r2,r3      -> cmp r2,r2     (Parent-Vergleich neutral)
  0x212F40  cmpeq sb,r6     -> cmp sb,sb      (Laengen-Vergleich neutral)
  0x212F44  b 0x399F4C      -> bne 0x212F5C   (Namelen-Cave umgehen)

Der anschliessende echte String-Vergleich (0x21077C) bleibt aktiv.
"""
import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000
PATCHES = [
    (0x00212F3C, 0xE1520002),   # cmp r2,r2
    (0x00212F40, 0xE15B000B),   # cmp sb,sb
    (0x00212F44, 0x1A000004),   # bne 0x212F5C (Original)
]

d = bytearray(open(PATH, 'rb').read())
for addr, val in PATCHES:
    off = addr - BASE
    old = struct.unpack_from('<I', d, off)[0]
    struct.pack_into('<I', d, off, val)
    print('%08X: %08X -> %08X' % (addr, old, val))
open(PATH, 'wb').write(d)
