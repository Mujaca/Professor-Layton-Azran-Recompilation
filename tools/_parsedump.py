d = {}
txt = open('out/dumpmeta_log.txt', encoding='utf-16').read()
for s in txt.splitlines():
    s = s.strip()
    if s.startswith('[range]') and '=' in s:
        try:
            left, right = s[7:].split('=')
            d[int(left.strip(), 16)] = int(right.strip(), 16)
        except ValueError:
            pass
b = bytearray()
for a in range(0x09000000, 0x09002000, 4):
    b += d.get(a, 0).to_bytes(4, 'little')
print('entries', len(d), 'bytes', len(b))
for pat in (b'lt6', b'arc', b'.fa', b'product', b'uk'):
    print(pat, b.find(pat), b.count(pat))
print(b[:64])
