import re
import sys
from collections import Counter

p = sys.argv[1]
data = open(p, 'rb').read()
if data[:2] == b'\xff\xfe':
    s = data.decode('utf-16-le', errors='replace')
else:
    s = data.decode('utf-8', errors='replace')
rows = re.findall(r'\[mw\] ([0-9A-F]{8}) <- ([0-9A-F]{8}) size=(\d+) pc=([0-9A-F]{8}) lr=([0-9A-F]{8})', s)
print('mw rows', len(rows))
c = Counter((r[3], r[4]) for r in rows)
for (pc, lr), n in c.most_common(20):
    print('pc=%s lr=%s n=%d' % (pc, lr, n))
print('--- writes from 0x27xxxx/0x28xxxx (builder) ---')
seen = 0
for addr, val, size, pc, lr in rows:
    if pc.startswith('001') or pc.startswith('000'):
        continue
    print('%s <- %s size=%s pc=%s lr=%s' % (addr, val, size, pc, lr))
    seen += 1
    if seen >= 40:
        break
