import io, re

L = [l.rstrip() for l in io.open('out/boot_log.txt', encoding='utf-8', errors='replace')
     if 'wait25 handles' in l]
s = set()
for l in L:
    m = re.search(r'th=(\d+) n=(\d+): (.*)$', l)
    if m:
        s.add((m.group(1), m.group(2), m.group(3)))
print('distinct', len(s))
for x in sorted(s):
    print(x)
