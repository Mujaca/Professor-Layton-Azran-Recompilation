"""RomFS-Kette reparieren: Zyklen entfernen.

Der fruehere Ketten-Patch hat Bucket->Record UND record.hashnext->alter Kopf
gesetzt. Da der alte Kopf die Kette bis genau zu diesem Record fuehrt,
entstand ein Zyklus (Engine-Schleife bei 0x212D70).

Fix: Bucket wieder auf den alten Kopf, hashnext des Records auf FFFFFFFF.
Der Record ist damit am Kettenende erreichbar (kein Zyklus).

Idempotent; aendert nur out/romfs_image.bin.
"""

import struct

PATH = 'out/romfs_image.bin'
BASE = 0x1000
FILE_META = BASE + 0xAF4

# (bucket-offset, original-kopf, record-offset)
PATCHES = [
    (0x12C8, 0x2060, 0x0948),   # /lt6/snd/uk/product.bcsar
    (0x117C, 0x8CD8, 0x0000),   # /lt6/arc/lt6_a.fa
    (0x1A74, 0x7248, 0x01D8),   # /lt6/mov/m001.moflex
    (0x1504, 0x2D70, 0x0984),   # BGM_001
    (0x1640, 0x5498, 0x09D0),   # BGM_002
]


def main():
    d = bytearray(open(PATH, 'rb').read())
    for bucket_off, head, rec in PATCHES:
        hoff = FILE_META + rec + 0x18
        struct.pack_into('<I', d, bucket_off, head)
        struct.pack_into('<I', d, hoff, 0xFFFFFFFF)
        print('%06X -> %08X ; hashnext@%06X -> FFFFFFFF'
              % (bucket_off, head, hoff))
    open(PATH, 'wb').write(d)


if __name__ == '__main__':
    main()
