"""RomFS-File-Hashketten komplett neu aufbauen.

Hintergrund: fruehere Patches (Bucket umbiegen + hashnext) haben Zyklen
bzw. abgeschnittene Ketten erzeugt. Dieses Skript berechnet fuer JEDEN
File-Record den korrekten Bucket (Hash aus Name + Parent-Offset) und baut
daraus saubere, FFFFFFFF-terminierte Ketten. Bucket-Tabelle und hashnext
werden konsistent geschrieben.

Idempotent; aendert nur out/romfs_image.bin.
"""

import struct

PATH = 'out/romfs_image.bin'
BASE = 0x1000
FILE_HASH = BASE + 0x160
FILE_META = BASE + 0xAF4
FILE_TOTAL = 0xD284
COUNT = 0x994 // 4

PATCHES = []


def rhash(name, seed):
    h = seed ^ 0x75BCD15
    for ch in name:
        h = (ord(ch) ^ (((h >> 5) | (h << 27)) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return h


def main():
    d = bytearray(open(PATH, 'rb').read())

    # Records sequentiell einsammeln
    recs = []
    off = 0
    while off + 0x20 <= FILE_TOTAL:
        base = FILE_META + off
        parent = struct.unpack_from('<I', d, base)[0]
        nlen = struct.unpack_from('<I', d, base + 0x1C)[0]
        if nlen > 0x200:
            break
        raw = bytes(d[base + 0x20:base + 0x20 + nlen])
        name = raw.decode('utf-16-le', 'replace')
        recs.append((off, parent, name))
        off += 0x20 + ((nlen + 3) & ~3)


    head = [0xFFFFFFFF] * COUNT
    nxt = {}
    for off, parent, name in recs:
        b = rhash(name, parent) % COUNT
        if head[b] == 0xFFFFFFFF:
            head[b] = off
        else:
            prev = head[b]
            while nxt.get(prev, 0xFFFFFFFF) != 0xFFFFFFFF:
                prev = nxt[prev]
            nxt[prev] = off
        nxt.setdefault(off, 0xFFFFFFFF)

    for b in range(COUNT):
        struct.pack_into('<I', d, FILE_HASH + b * 4, head[b])
    for off, parent, name in recs:
        struct.pack_into('<I', d, FILE_META + off + 0x18, nxt[off])

    open(PATH, 'wb').write(d)
    print('records=%d, buckets=%d neu aufgebaut' % (len(recs), COUNT))


if __name__ == '__main__':
    main()
