from PIL import Image

im = Image.open(r'out\shot_chain_direct.bmp').convert('RGB')
px = im.load()
ref = open(r'out\menu_bottom.bin', 'rb').read()

for ox in (0, 20, 40, 50, 60):
    diff = 0
    tot = 0
    for y in range(240):
        for x in range(320):
            sx = x
            r = ref[(y * 320 + sx) * 3:(y * 320 + sx) * 3 + 3]
            p = px[x + ox, y + 240]
            tot += 1
            if abs(r[0] - p[0]) + abs(r[1] - p[1]) + abs(r[2] - p[2]) > 20:
                diff += 1
    print('ox=%d diff>20 %d/%d' % (ox, diff, tot))
