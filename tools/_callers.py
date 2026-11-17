import io, re, sys

L = io.open('build/gen_boot.c', encoding='utf-8').read().split('\n')

def func_at(i):
    for j in range(i, -1, -1):
        m = re.match(r'void (fn_[0-9A-F]{8})\(ArmCpu \*c\) \{', L[j])
        if m:
            return m.group(1)
    return None

target = sys.argv[1] if len(sys.argv) > 1 else '001083B4'
pat = 'fn_%s(c);' % target
callers = []
for i, l in enumerate(L):
    if pat in l:
        f = func_at(i)
        if f:
            callers.append((f, l.strip()))
seen = []
for f, l in callers:
    if f not in seen:
        seen.append(f)
print('callers of', target, ':', len(seen))
for f in seen:
    print('  ', f)
