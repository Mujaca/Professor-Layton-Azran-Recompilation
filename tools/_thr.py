import io, re

E = [l.rstrip() for l in io.open('out/boot_err.txt', encoding='utf-8', errors='replace')]
marks = [i for i, l in enumerate(E) if 'tdump bei tick' in l]
print('dumps', marks)
k = marks[-1]
cur = None
per = {}
j = k + 1
while j < len(E) and E[j].startswith('[tdump]'):
    m = re.match(r'\[tdump\] thread (\w+) letzte', E[j])
    if m:
        cur = m.group(1)
        per[cur] = []
    else:
        m2 = re.match(r'\[tdump\]\s+\d+ pc=(\w+)', E[j])
        if m2 and cur is not None:
            per[cur].append(int(m2.group(1), 16))
    j += 1
for t, pcs in per.items():
    app = [hex(p) for p in pcs if 0x100000 <= p < 0x150000]
    print('thread', t, 'n', len(pcs), 'app', len(app), app[:8])
