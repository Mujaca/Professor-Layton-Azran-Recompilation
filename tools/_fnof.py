import io
import re

L = io.open('build/gen_boot.c', encoding='utf-8').read().split('\n')
starts = []
for i, l in enumerate(L):
    m = re.match(r'void (fn_([0-9A-F]{8}))\(ArmCpu \*c\) \{', l)
    if m:
        starts.append((int(m.group(2), 16), m.group(1), i))

def enclosing(addr):
    best = None
    for a, f, i in starts:
        if a <= addr:
            best = (a, f, i)
        else:
            break
    return best

for t in (0x11C458, 0x278374, 0x1234FC, 0x11C45C):
    print(hex(t), enclosing(t))
