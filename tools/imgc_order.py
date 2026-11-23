"""IMGC00-Decode mit waehlbarer Tile-Ordnung (Diagnose fuer 000.xi).

`_imgc.decode` wendet die 8x8-Makro-Z-Ordnung (`master_get`) an. Bei
title_b/000.xi sehen die Textzeilen danach verschoben aus (Buchstaben aus
8-px-Bloecken). Dieses Tool baut das Tile-Bild direkt aus der Tabelle und
kann die Pixel wahlweise linear oder mit Z-Ordnung zusammensetzen.

Aufruf: python tools/imgc_order.py <file.xi> <linear|z> [ausgabe.bin]
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc  # noqa: E402


def load(path):
    d = open(path, 'rb').read()
    bitdepth = d[0x0D]
    bpt = struct.unpack_from('<h', d, 0x0E)[0]
    w = struct.unpack_from('<h', d, 0x10)[0]
    h = struct.unpack_from('<h', d, 0x12)[0]
    toff = struct.unpack_from('<i', d, 0x1C)[0]
    ts1 = struct.unpack_from('<i', d, 0x34)[0]
    ts2 = struct.unpack_from('<i', d, 0x38)[0]
    isz = struct.unpack_from('<i', d, 0x3C)[0]
    table = _imgc.decomp(d[toff:toff + ts1])
    tex = _imgc.decomp(d[toff + ts2:toff + ts2 + isz])
    pic = bytearray()
    pos = 0
    entry_len = 2
    if len(table) >= 2 and struct.unpack_from('<H', table, 0)[0] == 0x453:
        pos = 8
        entry_len = 4
    while pos + entry_len <= len(table):
        e = (struct.unpack_from('<H', table, pos)[0] if entry_len == 2
             else struct.unpack_from('<I', table, pos)[0])
        pos += entry_len
        if e in (0xFFFF, 0xFFFFFFFF):
            pic.extend(b'\0' * bpt)
        else:
            o = e * bpt
            pic.extend(tex[o:o + bpt])
    return bitdepth, bpt, w, h, pic


def decode(path, order='linear'):
    bitdepth, bpt, w, h, pic = load(path)
    bpp = bitdepth // 8
    w8 = (w + 7) & ~7
    out = bytearray(w * h * bpp)
    for y in range(h):
        for x in range(w):
            if order == 'linear':
                sx, sy = x, y
            else:
                sx, sy = _imgc.master_get(y * w8 + x, w8)
            o = (sy * w8 + sx) * bpp
            out[(y * w + x) * bpp:(y * w + x + 1) * bpp] = \
                pic[o:o + bpp] if o + bpp <= len(pic) else b'\0' * bpp
    return w, h, bpp, bytes(out)


def main():
    path, order = sys.argv[1], sys.argv[2]
    w, h, bpp, out = decode(path, order)
    print('%s order=%s %dx%d bpp=%d' % (os.path.basename(path), order, w, h, bpp))
    if len(sys.argv) > 3:
        open(sys.argv[3], 'wb').write(out)
        print('->', sys.argv[3])


if __name__ == '__main__':
    main()
