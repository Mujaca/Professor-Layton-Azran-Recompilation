import re

d = open('out/exefs/code_decomp.bin', 'rb').read()
g = open('build/gen_boot.c', encoding='utf-8', errors='replace').read()
funcs = sorted(set(int(a, 16) for a in
                   re.findall(r'\{ 0x([0-9A-F]{8})u, fn_', g)))


def owner(addr):
    best = None
    for f in funcs:
        if f <= addr:
            best = f
        else:
            break
    return best


def scan(value):
    b = value.to_bytes(4, 'little')
    out = []
    i = 0
    while True:
        j = d.find(b, i)
        if j < 0:
            break
        out.append(0x100000 + j)
        i = j + 1
    return out


for v in (0x00060040, 0x000C0040, 0x000600C4, 0x000C00C4,
          0x000600C0, 0x000C00C0):
    hits = scan(v)
    print(hex(v), 'hits', len(hits))
    for a in hits[:12]:
        print('   literal at %08X owner=%s' % (a, owner(a)))
