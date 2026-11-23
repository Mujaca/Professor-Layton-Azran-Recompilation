import sys

path = sys.argv[1]
w = int(sys.argv[2])
h = int(sys.argv[3])
d = open(path, 'rb').read()
cols = int(sys.argv[4]) if len(sys.argv) > 4 else 48
rows = max(8, cols * h // (w * 2))
chars = ' .:-=+*#%@'
for gy in range(rows):
    y0 = gy * h // rows
    y1 = max(y0 + 1, (gy + 1) * h // rows)
    line = ''
    for gx in range(cols):
        x0 = gx * w // cols
        x1 = max(x0 + 1, (gx + 1) * w // cols)
        s = 0
        n = 0
        for y in range(y0, y1, 2):
            for x in range(x0, x1, 2):
                i = (y * w + x) * 3
                s += (d[i] + d[i + 1] + d[i + 2]) // 3
                n += 1
        line += chars[min(9, (s // max(1, n)) * 10 // 256)]
    print(line)
