import re

lines = open('build/gen_boot.c', encoding='utf-8').read().split('\n')
start = None
for i, l in enumerate(lines):
    if l.startswith('void fn_002808A8(ArmCpu *c) {'):
        start = i
        break
end = start
for i in range(start + 1, len(lines)):
    if lines[i].startswith('void fn_'):
        end = i
        break
body = lines[start:end]
print('lines', start, end, 'count', len(body))
for i, l in enumerate(body):
    if 'r[13]' in l or 'arm_ldm_stm' in l:
        print('%5d %s' % (start + i, l.strip()))
