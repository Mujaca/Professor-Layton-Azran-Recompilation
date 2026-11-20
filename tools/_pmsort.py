rows = []
for line in open('out/perfmap2.txt'):
    if line.startswith('pc '):
        parts = line.split()
        rows.append((int(parts[2]), int(parts[3]), parts[1]))

for th in (2, 4, 5, 6, 10):
    top = sorted([r for r in rows if r[0] == th], key=lambda r: -r[1])[:8]
    print('thread', th, [(r[2], r[1]) for r in top])
