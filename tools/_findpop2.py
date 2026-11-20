import re

lines = open('build/gen_boot.c', encoding='utf-8').read().split('\n')


def find(fn):
    start = None
    for i, l in enumerate(lines):
        if l.startswith('void %s(ArmCpu *c) {' % fn):
            start = i
            break
    cur = None
    out = []
    for i in range(start, len(lines)):
        l = lines[i]
        if l.startswith('void fn_') and i > start:
            break
        m = re.search(r'arm_trace_block\(c, 0x([0-9A-F]+)u', l)
        if m:
            cur = m.group(1)
        if 'arm_ldm_stm' in l:
            out.append((cur, l.strip()))
    return out


for fn in ('fn_002808A8', 'fn_00280914'):
    print('==', fn)
    for c, l in find(fn):
        print('  block', c, l)
