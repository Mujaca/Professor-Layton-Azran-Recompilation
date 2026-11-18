import io
import re

L = io.open('build/gen_boot.c', encoding='utf-8').read().split('\n')
starts = []
for i, l in enumerate(L):
    m = re.match(r'void (fn_([0-9A-F]{8}))\(ArmCpu \*c\) \{', l)
    if m:
        starts.append((int(m.group(2), 16), i, m.group(1)))

tgt = 0x11A818
best = None
for a, i, f in starts:
    if a <= tgt:
        best = (a, i, f)
    else:
        break
print(best)
a, i, f = best
# Ende der Funktion finden
j = i + 1
while j < len(L) and not L[j].startswith('void fn_'):
    j += 1
print('lines', i, '..', j)
for k in range(i, min(j, i + 60)):
    print(k, L[k])
