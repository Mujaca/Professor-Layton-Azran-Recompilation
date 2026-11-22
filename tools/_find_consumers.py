import re
p = 'build/gen_boot.c'
txt = open(p, encoding='utf-8', errors='replace').read()
parts = re.split(r'(?m)^(void fn_[0-9A-F]+\(ArmCpu \*c\) \{)', txt)
res = []
for i in range(1, len(parts), 2):
    h = parts[i]
    b = parts[i + 1]
    if '+ 0x0000006Cu' in b and '+ 0x00000074u' in b:
        addr = re.search(r'fn_([0-9A-F]+)', h).group(1)
        res.append((addr, b.count('+ 0x0000006Cu'), b.count('+ 0x00000074u')))
print(len(res))
for a, c1, c2 in res[:60]:
    print(a, c1, c2)
