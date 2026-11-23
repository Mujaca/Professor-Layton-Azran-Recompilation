from PIL import Image
import os

shot = r'out\shot_chain_direct.bmp'
im = Image.open(shot).convert('RGB')
px = im.load()
top = bytes([c for y in range(240) for x in range(400) for c in px[x, y]])
bot = bytes([c for y in range(240, 480) for x in range(320) for c in px[x, y]])

pairs = ((r'out\menu_top.bin', top),
         (r'reports\menu_official.bin', top),
         (r'out\menu_bottom.bin', bot),
         (r'reports\menu_official_bottom.bin', bot))
for f, data in pairs:
    if not os.path.exists(f):
        print('%s fehlt' % f)
        continue
    d = open(f, 'rb').read()
    n = min(len(d), len(data))
    diff = 0
    tot = 0
    for i in range(0, n, 3):
        tot += 1
        if abs(d[i] - data[i]) + abs(d[i + 1] - data[i + 1]) + \
                abs(d[i + 2] - data[i + 2]) > 60:
            diff += 1
    print('%s len=%d diff>60 %d/%d' % (f, len(d), diff, tot))
    print('  erste 3 px:', list(d[0:9]))

print('shot top erste 3 px:', list(top[0:9]))
print('shot bot erste 3 px:', list(bot[0:9]))
