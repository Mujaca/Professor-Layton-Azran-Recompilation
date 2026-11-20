import re

lines = open('build/gen_boot.c', encoding='utf-8').read().split('\n')
start = None
for i, l in enumerate(lines):
    if l.startswith('void fn_00292650(ArmCpu *c) {'):
        start = i
        break
print('start', start)
for i in range(start, len(lines)):
    l = lines[i]
    if l.startswith('void fn_') and i > start:
        print('end', i, l.strip())
        break
    m = re.search(r'arm_trace_block\(c, 0x([0-9A-F]+)u', l)
    if m:
        print('block', m.group(1))
