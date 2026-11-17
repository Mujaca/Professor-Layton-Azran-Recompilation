"""ETC1/ETC1A4-Decode des Banner-CGFX; schreibt out/menu_frame.bin (400x240)."""
import struct, zlib

d = open('out/banner_cgfx.bin', 'rb').read()
DATA = 0x8A64
W, H = 512, 256
OW, OH = 400, 240

MODS = ((2, 8, -2, -8), (5, 17, -5, -17), (9, 29, -9, -29), (13, 42, -13, -42),
        (18, 60, -18, -60), (24, 80, -24, -80), (33, 106, -33, -106), (47, 183, -47, -183))

def expand4(v):
    return (v << 4) | v

def decode_etc1(off, etc1a4=False):
    out = bytearray(W * H * 3)
    p = off
    for by in range(0, H, 4):
        for bx in range(0, W, 4):
            high = int.from_bytes(d[p:p+4], 'big')
            low = int.from_bytes(d[p+4:p+8], 'big')
            p += 8
            if etc1a4:
                p += 8   # Alpha-Block (nicht benutzt)
            diff = (high >> 1) & 1
            flip = high & 1
            table = (high >> 5) & 7
            if diff:
                def s5(v):
                    v &= 0x1F
                    return v - 32 if v & 0x10 else v
                r1 = (high >> 27) & 0x1F; g1 = (high >> 19) & 0x1F; b1 = (high >> 11) & 0x1F
                r2 = r1 + s5(high >> 3); g2 = g1 + s5(low >> 27); b2 = b1 + s5(low >> 19)
                r1 = (r1 << 3) | (r1 >> 2); g1 = (g1 << 3) | (g1 >> 2); b1 = (b1 << 3) | (b1 >> 2)
                r2 = max(0, min(31, r2)); g2 = max(0, min(31, g2)); b2 = max(0, min(31, b2))
                r2 = (r2 << 3) | (r2 >> 2); g2 = (g2 << 3) | (g2 >> 2); b2 = (b2 << 3) | (b2 >> 2)
            else:
                r1 = expand4((high >> 28) & 0xF); g1 = expand4((high >> 20) & 0xF); b1 = expand4((high >> 12) & 0xF)
                r2 = expand4((high >> 4) & 0xF); g2 = expand4((low >> 28) & 0xF); b2 = expand4((low >> 20) & 0xF)
            for i in range(16):
                x = i // 4
                y = i % 4
                msb = (low >> (16 + i)) & 1
                lsb = (low >> i) & 1
                m = MODS[table][(msb << 1) | lsb]
                sub2 = (x >= 2) if flip else (y >= 2)
                br, bg, bb = (r2, g2, b2) if sub2 else (r1, g1, b1)
                r = br + m; g = bg + m; b = bb + m
                r = 0 if r < 0 else (255 if r > 255 else r)
                g = 0 if g < 0 else (255 if g > 255 else g)
                b = 0 if b < 0 else (255 if b > 255 else b)
                o = ((by + y) * W + bx + x) * 3
                out[o] = r; out[o+1] = g; out[o+2] = b
    return out

def score(img):
    n = W * H
    mean = sum(img) / (3.0 * n)
    smooth = 0
    cnt = 0
    for y in range(0, H, 4):
        row = y * W * 3
        for x in range(0, W - 1, 2):
            smooth += abs(img[row + x*3] - img[row + (x+1)*3])
            cnt += 1
    return mean, smooth / float(cnt)

best = None
for etc1a4 in (False,):
    for off in (DATA,):
        img = decode_etc1(off, etc1a4)
        m, s = score(img)
        print('a4=%d off %06X mean %6.1f smooth %5.1f' % (etc1a4, off, m, s))
        best = (s, off, etc1a4, img)

s, off, a4, img = best
print('gewaehlt off %06X a4=%d smooth %.1f' % (off, a4, s))

out = bytearray(OW * OH * 3)
for y in range(OH):
    sy = y * H // OH
    for x in range(OW):
        sx = x * W // OW
        o = (sy * W + sx) * 3
        q = (y * OW + x) * 3
        out[q:q+3] = img[o:o+3]
open('out/menu_frame.bin', 'wb').write(out)

def png(path, raw, w, h):
    rows = b''.join(b'\x00' + bytes(raw[y*w*3:(y+1)*w*3]) for y in range(h))
    def chunk(t, data):
        return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xFFFFFFFF)
    o = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    o += chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(o)

png('out/menu_frame.png', out, OW, OH)
print('geschrieben out/menu_frame.png')
