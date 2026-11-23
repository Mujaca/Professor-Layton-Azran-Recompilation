import re

s = open('build/gen_boot.c', encoding='utf-8', errors='replace').read()

defs = [(m.start(), m.group(1)) for m in
        re.finditer(r'void (fn_[0-9A-F]+)\(ArmCpu \*c\) \{', s)]


def owner(pos):
    best = None
    for p, name in defs:
        if p <= pos:
            best = name
        else:
            break
    return best


pat = re.compile(r'uint32_t a = c->r\[(\d+)\] \+ 0x0000006Au;\s*\n\s*arm_st\(c, a, (\d)')
print('stores to +0x6A:', len(pat.findall(s)))
seen = {}
for m in pat.finditer(s):
    name = owner(m.start())
    seen.setdefault(name, []).append(m.group(1) + '/' + m.group(2))
for name, hits in list(seen.items())[:20]:
    print(name, hits[:4])

pat2 = re.compile(r'uint32_t a = c->r\[(\d+)\] \+ 0x0000005Cu;\s*\n\s*arm_st\(c, a, (\d)')
print('stores to +0x5C:', len(pat2.findall(s)))
seen2 = {}
for m in pat2.finditer(s):
    name = owner(m.start())
    seen2.setdefault(name, []).append(m.group(1) + '/' + m.group(2))
for name, hits in list(seen2.items())[:20]:
    print(name, hits[:4])
