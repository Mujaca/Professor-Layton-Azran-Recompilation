"""Dekodiert das Banner-CGFX (ETC1) und schreibt out/menu_frame.bin (400x240 RGB)."""
import struct, zlib

d = open('out/banner_cgfx.bin', 'rb').read()
DATA = 0x8A64
W, H = 512, 256
OUT_W, OUT_H = 400, 240

def decode(off):
    out = bytearray(W * H * 3)
    p = off
    for by in range(0, H, 4):
        for bx in range(0, W, 4):
            hi = int.from_bytes(d[p:p+4], 'little')
            lo = int.from_bytes(d[p+4:p+8], 'little')
            p += 8
            if hi & 2:
                def s5(v):
                    v &= 0x1F
                    return v - 32 if v & 0x10 else v
                r1 = (hi >> 27) & 0x1F; g1 = (hi >> 19) & 0x1F; b1 = (hi >> 11) & 0x1F
                r2 = r1 + s5(hi >> 3); g2 = g1 + s5(lo >> 27); b2 = b1 + s5(lo >> 19)
                r1 = (r1 << 3) | (r1 >> 2); g1 = (g1 << 3) | (g1 >> 2); b1 = (b1 << 3) | (b1 >> 2)
                r2 = max(0, min(31, r2)); g2 = max(0, min(31, g2)); b2 = max(0, min(31, b2))
                r2 = (r2 << 3) | (r2 >> 2); g2 = (g2 << 3) | (g2 >> 2); b2 = (b2 << 3) | (b2 >> 2)
                bits = (hi >> 5) | ((lo & 0x7) << 27)
                flip = (hi >> 4) & 1
                t1 = (2, 8, -2, -8, 0, 0, 0, 0)
                t2 = (2, 8, -2, -8, 0, 0, 0, 0)
            else:
                r1 = ((hi >> 28) & 0xF) * 17; g1 = ((hi >> 20) & 0xF) * 17; b1 = ((hi >> 12) & 0xF) * 17
                r2 = ((hi >> 4) & 0xF) * 17; g2 = ((lo >> 28) & 0xF) * 17; b2 = ((lo >> 20) & 0xF) * 17
                bits = (hi >> 5) | ((lo & 0x7) << 27)
                flip = (hi >> 4) & 1
                t1 = (2, 8, -2, -8, 0, 0, 0, 0)
                t2 = (2, 8, -2, -8, 0, 0, 0, 0)
            for i in range(16):
                k = (bits >> (i * 2)) & 3
                base_r, base_g, base_b = (r2, g2, b2) if flip else (r1, g1, b1)
                tab = t2 if flip else t1
                m = tab[k] if k < 4 else tab[4 + (k & 3)]
                r = base_r + m; g = base_g + m; b = base_b + m
                r = 0 if r < 0 else (255 if r > 255 else r)
                g = 0 if g < 0 else (255 if g > 255 else g)
                b = 0 if b < 0 else (255 if b > 255 else b)
                x = bx + (i & 3); y = by + (i >> 2)
                o = (y * W + x) * 3
                out[o] = r; out[o+1] = g; out[o+2] = b
    return out

def score(img):
    colors = set()
    tot = 0
    for i in range(0, len(img), 3 * 41):
        colors.add((img[i], img[i+1], img[i+2]))
        tot += img[i] + img[i+1] + img[i+2]
    smooth = 0
    for y in range(0, H, 8):
        row = y * W * 3
        for x in range(0, W - 1, 3):
            smooth += abs(img[row + x*3] - img[row + (x+1)*3])
    return len(colors), tot / (len(img) / 3.0), smooth

cands = [DATA, DATA + 0x800, DATA + 0x1000, DATA + 0x2000, 0x2A880]
best = None
for off in cands:
    if off + W * H // 2 > len(d):
        continue
    img = decode(off)
    nc, mean, smooth = score(img)
    print('off %06X colors %5d mean %6.1f smooth %8d' % (off, nc, mean, smooth))
    if best is None or nc > best[1]:
        best = (off, nc, img)

off, nc, img = best
print('gewaehlt:', hex(off), 'Farben', nc)

# auf 400x240 herunterskalieren (Box-Sampling)
out = bytearray(OUT_W * OUT_H * 3)
for y in range(OUT_H):
    sy0 = y * H // OUT_H; sy1 = max(sy0 + 1, (y + 1) * H // OUT_H)
    for x in range(OUT_W):
        sx0 = x * W // OUT_W; sx1 = max(sx0 + 1, (x + 1) * W // OUT_W)
        r = g = b = n = 0
        for yy in range(sy0, sy1):
            for xx in range(sx0, sx1):
                o = (yy * W + xx) * 3
                r += img[o]; g += img[o+1]; b += img[o+2]; n += 1
        o = (y * OUT_W + x) * 3
        out[o] = r // n; out[o+1] = g // n; out[o+2] = b // n
open('out/menu_frame.bin', 'wb').write(out)
print('geschrieben out/menu_frame.bin', len(out))

def png(path, raw, w, h):
    rows = b''.join(b'\x00' + bytes(raw[y*w*3:(y+1)*w*3]) for y in range(h))
    def chunk(t, data):
        return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xFFFFFFFF)
    o = b'\x89PNG\r\n\x1a\n'
    o += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    o += chunk(b'IDAT', zlib.compress(rows, 9))
    o += chunk(b'IEND', b'')
    open(path, 'wb').write(o)

png('out/menu_frame.png', out, OUT_W, OUT_H)
print('geschrieben out/menu_frame.png')
