p = 'build/gen_boot.c'
s = open(p, encoding='utf-8', errors='replace').read()

if 'b36dbg3' in s:
    print('already')
else:
    pairs = [('0010F8C4', '0x00103968u', 'gate3'),
             ('0010F98C', '0x0010397Cu', 'gate4')]
    for fn, ret, tag in pairs:
        old = ('  { uint32_t sp0 = c->r[13];\n'
               '    c->r[14] = ' + ret + ';\n'
               '    fn_' + fn + '(c);\n'
               '    arm_restore_sp(c, sp0); }\n')
        if old not in s:
            print('missing', tag)
            continue
        add = old + ('  { static int bd3=0; if (bd3<4) { bd3++; '
                     'printf("[b36dbg3] ' + tag + ' r0=%u\\n", '
                     '(unsigned)c->r[0]); fflush(stdout); } }\n')
        s = s.replace(old, add, 1)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
