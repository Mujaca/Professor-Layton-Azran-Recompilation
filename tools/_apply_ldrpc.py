"""Fuegt ldr-pc-Logging (arm_ldrpc_log) in Runtime+Recompiler ein."""
p = r'runtime\arm_trace.h'
s = open(p, encoding='utf-8').read()
if 'arm_ldrpc_log' not in s:
    i = s.find('struct ArmCpu;')
    ins = 'int arm_ldrpc_log_on(void);\nvoid arm_ldrpc_log(unsigned t);\n'
    s = s[:i] + ins + s[i:] if i >= 0 else s + '\n' + ins
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('arm_trace.h patched')

p2 = r'runtime\arm_trace.c'
t = open(p2, encoding='utf-8').read()
if 'arm_ldrpc_log_on' not in t:
    impl = (
        'int arm_ldrpc_log_on(void)\n'
        '{\n'
        '    static int on = -1;\n'
        '    if (on < 0) on = envset("HLE_LDRPC_LOG");\n'
        '    return on;\n'
        '}\n'
        'void arm_ldrpc_log(unsigned t)\n'
        '{\n'
        '    printf("[ldrpc] -> %08X\\n", t);\n'
        '    fflush(stdout);\n'
        '}\n'
    )
    i = t.find('int arm_retforce(struct ArmCpu *c, uint32_t addr) {')
    t = t[:i] + impl + t[i:]
    open(p2, 'w', encoding='utf-8', newline='\n').write(t)
    print('arm_trace.c patched')

p3 = r'tools\recomp\emit.py'
e = open(p3, encoding='utf-8').read()
old = '            lines.append("arm_call_guest(c, c->r[15]);")'
new = ('            lines.append("if (arm_ldrpc_log_on()) arm_ldrpc_log(c->r[15]);")\n'
       '            lines.append("arm_call_guest(c, c->r[15]);")')
if old in e and 'arm_ldrpc_log_on' not in e:
    e = e.replace(old, new)
    open(p3, 'w', encoding='utf-8', newline='\n').write(e)
    print('emit.py patched')
print('done')
