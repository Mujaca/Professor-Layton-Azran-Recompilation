ref = open('reports/menu_official.bin', 'rb').read()
W, H = 400, 240
xs = []
ys = []
for y in range(H):
    for x in range(W):
        i = (y * W + x) * 3
        r, g, b = ref[i], ref[i + 1], ref[i + 2]
        if r > 200 and g > 200 and b > 180:
            xs.append(x)
            ys.append(y)
print('bright pixels', len(xs))
if xs:
    print('bbox', min(xs), min(ys), max(xs), max(ys))
