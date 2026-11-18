import io
import re
import collections

A = []
for l in io.open('out/boot_log.txt', encoding='utf-8', errors='replace'):
    if l.startswith('[arb]'):
        A.append(l.rstrip())
print('arb lines', len(A))

c = collections.Counter()
for l in A:
    m = re.search(r'addr=(\w+) type=(\d+) val=(\d+) mem=(\w+)', l)
    if m:
        c[(m.group(1), m.group(2), m.group(4))] += 1
print('distinct', len(c))
for k, v in c.most_common(20):
    print(v, 'addr=%s type=%s mem=%s' % k)

print('=== tail arb ===')
for l in A[-12:]:
    print(l)

print('=== distinct wait25 ===')
W = set()
for l in io.open('out/boot_log.txt', encoding='utf-8', errors='replace'):
    if l.startswith('[wait25 handles]'):
        W.add(l.rstrip())
for l in sorted(W):
    print(l)
