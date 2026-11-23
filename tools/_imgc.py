"""Decoder fuer Level-5 IMGC00 (.xi). Nutzt tools/xfsatool/dectool.exe
fuer LZSS/Huffman und rekonstruiert Tile-Ordnung + Imgc-Z-Order."""
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DECT = os.path.join(ROOT, 'tools', 'xfsatool', 'dectool.exe')

BIT_COORDS = [(0, 1), (1, 0), (0, 2), (2, 0), (0, 4), (4, 0)]


def master_get(point_count, width):
    macro_w = macro_h = 8
    macro_count = point_count // (macro_w * macro_h)
    width_tiles = (width + macro_w - 1) // macro_w
    mx = macro_count % width_tiles
    my = macro_count // width_tiles
    px, py = mx * macro_w, my * macro_h
    for j, (bx, by) in enumerate(BIT_COORDS):
        if (point_count >> j) & 1:
            px ^= bx
            py ^= by
    return px, py


def rle(data):
    """Level-5 RLE (method 4): flag byte; high bit -> repeat next byte
    (flag&0x7f)+3 times, else flag+1 literal bytes."""
    size = ((data[0] >> 3) | (data[1] << 5) | (data[2] << 13) |
            (data[3] << 21))
    out = bytearray()
    p = 4
    while len(out) < size and p < len(data):
        flag = data[p]
        p += 1
        if flag & 0x80:
            out += bytes([data[p]]) * ((flag & 0x7F) + 3)
            p += 1
        else:
            n = flag + 1
            out += data[p:p + n]
            p += n
    return bytes(out[:size])


def huffman(data, bit_depth):
    """Level-5 Huffman (method 2 = 4-bit, method 3 = 8-bit)."""
    size = ((data[0] >> 3) | (data[1] << 5) | (data[2] << 13) |
            (data[3] << 21))
    p = 4
    tree_size = data[p]
    tree_root = data[p + 1]
    p += 2
    tree = data[p:p + tree_size * 2]
    p += tree_size * 2

    n_out = size * 8 // bit_depth
    res = bytearray()
    pos = tree_root
    nxt = 0
    code = 0
    i = 0
    while len(res) < n_out:
        if i % 32 == 0:
            code = struct.unpack_from('<I', data, p)[0]
            p += 4
        nxt += ((pos & 0x3F) << 1) + 2
        direction = 2 if ((code >> (31 - (i % 32))) & 1) == 0 else 1
        leaf = ((pos >> (5 + direction)) & 1) != 0
        pos = tree[nxt - direction]
        if leaf:
            res.append(pos)
            pos = tree_root
            nxt = 0
        i += 1
    if bit_depth == 4:
        return bytes((res[2 * j] & 0xF) | ((res[2 * j + 1] & 0xF) << 4)
                     for j in range(size))
    return bytes(res)


def decomp(data):
    method = data[0] & 7
    if method == 4:  # Level-5 RLE
        return rle(data)
    if method == 2:  # Level-5 Huffman4Bit
        return huffman(data, 4)
    if method == 3:  # Level-5 Huffman8Bit
        return huffman(data, 8)
    if method == 0:  # uncompressed, 4-byte header
        return data[4:]
    with tempfile.TemporaryDirectory() as td:
        a = os.path.join(td, 'in.bin')
        b = os.path.join(td, 'out.bin')
        open(a, 'wb').write(data)
        r = subprocess.run([DECT, a, b], capture_output=True)
        if r.returncode != 0:
            raise RuntimeError('dectool %d' % r.returncode)
        return open(b, 'rb').read()


def decode(path):
    d = open(path, 'rb').read()
    assert d[:6] == b'IMGC00', (path, d[:6])
    bitdepth = d[0x0D]
    bytes_per_tile = struct.unpack_from('<h', d, 0x0E)[0]
    w = struct.unpack_from('<h', d, 0x10)[0]
    h = struct.unpack_from('<h', d, 0x12)[0]
    table_off = struct.unpack_from('<i', d, 0x1C)[0]
    table_size1 = struct.unpack_from('<i', d, 0x34)[0]
    table_size2 = struct.unpack_from('<i', d, 0x38)[0]
    img_size = struct.unpack_from('<i', d, 0x3C)[0]
    table = decomp(d[table_off:table_off + table_size1])
    tex = decomp(d[table_off + table_size2:table_off + table_size2 + img_size])

    pic = bytearray()
    pos = 0
    entry_len = 2
    if len(table) >= 2 and struct.unpack_from('<H', table, 0)[0] == 0x453:
        pos = 8
        entry_len = 4
    n = len(table)
    while pos + entry_len <= n:
        e = (struct.unpack_from('<H', table, pos)[0] if entry_len == 2
             else struct.unpack_from('<I', table, pos)[0])
        pos += entry_len
        if e in (0xFFFF, 0xFFFFFFFF):
            pic.extend(b'\0' * bytes_per_tile)
        else:
            o = e * bytes_per_tile
            pic.extend(tex[o:o + bytes_per_tile])

    bpp = bitdepth // 8
    w8 = (w + 7) & ~7
    out = bytearray(w * h * bpp)

    def src_px(x, y):
        idx = y * w8 + x
        sx, sy = master_get(idx, w8)
        o = (sy * w8 + sx) * bpp
        if o + bpp > len(pic):
            return b'\0' * bpp
        return pic[o:o + bpp]

    for y in range(h):
        for x in range(w):
            out[(y * w + x) * bpp:(y * w + x + 1) * bpp] = src_px(x, y)
    return w, h, bpp, out


def main():
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    w, h, bpp, data = decode(path)
    print('%s: %dx%d bpp=%d' % (path, w, h, bpp))
    if out:
        rgb = bytearray(w * h * 3)
        for i in range(w * h):
            if bpp >= 3:
                b, g, r = data[i * bpp + 0], data[i * bpp + 1], data[i * bpp + 2]
            else:
                r = g = b = data[i * bpp]
            rgb[i * 3:i * 3 + 3] = bytes((r, g, b))
        open(out, 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + bytes(rgb))
        print('geschrieben:', out)


if __name__ == '__main__':
    main()
