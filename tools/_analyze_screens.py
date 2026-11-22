from PIL import Image
from collections import Counter

for p in ['reports/azahar_topscreen.png', 'reports/azahar_bottomscreen.png']:
    im = Image.open(p).convert('RGB')
    w, h = im.size
    px = list(im.getdata())
    c = Counter(px)
    print('===', p, im.size, 'unique', len(c))
    for col, n in c.most_common(6):
        print('  ', col, n, round(100 * n / (w * h), 1), '%')
    # column / row variance from background to find screen area
    bg = c.most_common(1)[0][0]
    colscore = []
    for x in range(w):
        s = 0
        for y in range(h):
            r, g, b = im.getpixel((x, y))
            s += abs(r - bg[0]) + abs(g - bg[1]) + abs(b - bg[2])
        colscore.append(s)
    rowscore = []
    for y in range(h):
        s = 0
        for x in range(w):
            r, g, b = im.getpixel((x, y))
            s += abs(r - bg[0]) + abs(g - bg[1]) + abs(b - bg[2])
        rowscore.append(s)
    # report edges where score jumps
    def edges(scores):
        mn = min(scores); mx = max(scores)
        thr = mn + (mx - mn) * 0.20
        idx = [i for i, v in enumerate(scores) if v > thr]
        return (idx[0], idx[-1]) if idx else (None, None)
    print('  col_active', edges(colscore), 'row_active', edges(rowscore))

# save a crop of the bottom screen and a text color grid
b = Image.open('reports/azahar_bottomscreen.png').convert('RGB')
crop = b.crop((56, 0, 56 + 320, 223))
canvas = Image.new('RGB', (320, 240), (0, 0, 0))
canvas.paste(crop, (0, 0))
canvas.save('out/shots/bottom_crop_320x240.png')
print('saved out/shots/bottom_crop_320x240.png')

# coarse text grid of bottom (each cell = 20x15 px)
gw, gh = 16, 12
for gy in range(gh):
    row = ''
    for gx in range(gw):
        x0 = int(gx * 320 / gw); x1 = int((gx + 1) * 320 / gw)
        y0 = int(gy * 240 / gh); y1 = int((gy + 1) * 240 / gh)
        rs = gs = bs = n = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                r, g, bb = canvas.getpixel((x, y))
                rs += r; gs += g; bs += bb; n += 1
        r, g, bb = rs // n, gs // n, bs // n
        lum = (r + g + bb) // 3
        row += ' .:-=+*#%@'[min(9, lum * 10 // 256)]
    print(row)
