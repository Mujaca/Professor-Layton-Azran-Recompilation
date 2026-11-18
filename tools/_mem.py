import io

p = 'runtime/hle/hle.c'
s = io.open(p, encoding='utf-8').read()

for n in (0x13, 0x14, 0x15, 0x1F, 0x20):
    pat = 'case 0x%02X:' % n
    print(pat, s.count(pat))
