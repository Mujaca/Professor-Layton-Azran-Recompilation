"""RomFS-Lookup reparieren (v2) und offline verifizieren.

Layout (aus den Metadaten verifiziert):
  Dir-Record : parent+0, sibling+4, childDir+8, childFile+0xC,
               HASHNEXT+0x10, nameLenBytes+0x14, name@+0x18
  File-Record: parent+0, sibling+4, dataOffset(u64)+8, dataSize(u64)+0x10,
               HASHNEXT+0x18, nameLenBytes+0x1C, name@+0x20

Die File-Hash-Buckets zeigen in eine Kette ueber HASHNEXT. 5 Records lagen
"vor" ihrem Bucket-Kopf und waren dadurch unerreichbar. Fix: Bucket auf den
Record umbiegen und HASHNEXT des Records auf den alten Kopf setzen; das
sibling-Feld (+4) bleibt unveraendert.
"""

import struct

PATH = 'out/romfs_image.bin'
BLOB = 0x1000
FILE_META = BLOB + 0xAF4
FILE_HASH = BLOB + 0x160
FILE_COUNT = 0x994 // 4

# (bucket-offset, alter kopf, record-offset, original sibling)
PATCHES = [
    (0x12C8, 0x2060, 0x0948, 0xFFFFFFFF),  # /lt6/snd/uk/product.bcsar
    (0x117C, 0x8CD8, 0x0000, 0x00000030),  # /lt6/arc/lt6_a.fa
    (0x1A74, 0x7248, 0x01D8, 0x00000210),  # /lt6/mov/m001.moflex
    (0x1504, 0x2D70, 0x0984, 0x000009D0),  # /lt6/snd/uk/stream/BGM_001
    (0x1640, 0x5498, 0x09D0, 0x00000A1C),  # /lt6/snd/uk/stream/BGM_002
]


def name_of(d, base, nlen_off, name_off):
    n = struct.unpack_from('<I', d, base + nlen_off)[0]
    raw = d[base + name_off: base + name_off + n]
    return raw.decode('utf-16-le', 'replace')


def rhash(name, seed):
    h = seed ^ 0x75BCD15
    for ch in name:
        h = (ord(ch) ^ (((h >> 5) | (h << 27)) & 0xFFFFFFFF)) & 0xFFFFFFFF
    return h


def main():
    d = bytearray(open(PATH, 'rb').read())
    for bucket_off, old_head, rec, sib in PATCHES:
        hoff = FILE_META + rec + 0x18
        struct.pack_into('<I', d, bucket_off, rec)
        struct.pack_into('<I', d, hoff, old_head)
        struct.pack_into('<I', d, FILE_META + rec + 4, sib)
    open(PATH, 'wb').write(d)
    print('5 Records an den Kettenanfang gehaengt (Hashnext -> alter Kopf).')

    # ---- verify ----
    d = open(PATH, 'rb').read()

    def u32(o):
        return struct.unpack_from('<I', d, o)[0]

    def frec(off):
        base = FILE_META + off
        nm = name_of(d, base, 0x1C, 0x20)
        return u32(base), u32(base + 4), u32(base + 0x18), nm

    bad = 0
    total = 0
    cur = 0
    # iterate file-meta sequentially via sibling chain starting at every dir's
    # first file is unreliable -> instead scan records sequentially by size
    off = 0
    while off < 0xD284:
        base = FILE_META + off
        parent, sib, hnext, nm = frec(off)
        nlen = u32(base + 0x1C)
        total += 1
        b = rhash(nm, parent) % FILE_COUNT
        c = u32(FILE_HASH + b * 4)
        found = False
        steps = 0
        while c != 0xFFFFFFFF and steps < 700:
            if c == off:
                found = True
                break
            c = u32(FILE_META + c + 0x18)
            steps += 1
        if not found:
            bad += 1
            if bad <= 10:
                print('MISS off=%04X %r parent=%04X bucket=%d head=%04X'
                         % (off, nm, parent, b, u32(FILE_HASH + b * 4)))
        off += 0x20 + ((nlen + 3) & ~3)
    print('Dateien: %d, nicht erreichbar: %d' % (total, bad))


if __name__ == '__main__':
    main()
