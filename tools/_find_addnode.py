import re

s = open('build/gen_boot.c', encoding='utf-8', errors='replace').read()
defs = [(m.start(), m.group(1)) for m in
        re.finditer(r'void (fn_[0-9A-F]+)\(ArmCpu \*c\) \{', s)]
defs.append((len(s), 'END'))

hits = []
for i in range(len(defs) - 1):
    p0, name = defs[i]
    p1 = defs[i + 1][0]
    seg = s[p0:p1]
    if '0x00000074u' in seg and '0x0000006Cu' in seg:
        hits.append((name, seg.count('0x00000074u'), seg.count('0x0000006Cu')))

print('candidates', len(hits))
for h in hits[:30]:
    print(h)
