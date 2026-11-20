import io
import re

addrs = []
L = io.open('build/gen_boot.c', encoding='utf-8').read().split(chr(10))
for l in L:
    m = re.match(r'void (fn_([0-9A-F]{8}))\(ArmCpu \*c\) \{', l)
    if m:
        addrs.append(int(m.group(2), 16))
addrs.sort()
for t in (0x126078, 0x1260E8, 0x12F784):
    i = max(k for k, a in enumerate(addrs) if a <= t)
    print(hex(t), '-> fn', hex(addrs[i]), '..', hex(addrs[i + 1]))
