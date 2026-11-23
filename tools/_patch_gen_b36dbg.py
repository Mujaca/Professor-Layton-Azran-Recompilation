p = 'build/gen_boot.c'
s = open(p, encoding='utf-8', errors='replace').read()

if 'b36dbg' in s:
    print('already')
else:
    pairs = [('0010FC9C', '0x001036C4u', 'gate1'),
             ('0010FBCC', '0x00103720u', 'gate2')]
    for fn, ret, tag in pairs:
        old = ('  { uint32_t sp0 = c->r[13];\n'
               '    c->r[14] = ' + ret + ';\n'
               '    fn_' + fn + '(c);\n'
               '    arm_restore_sp(c, sp0); }\n')
        if old not in s:
            print('missing', tag)
            continue
        add = old + ('  { static int bd=0; if (bd<4) { bd++; '
                     'printf("[b36dbg] ' + tag + ' r0=%u\\n", '
                     '(unsigned)c->r[0]); fflush(stdout); } }\n')
        s = s.replace(old, add, 1)
    key = ('L_001036B4_00103AF8:\n'
           '  arm_trace_block(c, 0x00103AF8u, 0u);\n')
    if key in s:
        s = s.replace(key, key + ('  { static int bf=0; if (bf<4) { bf++; '
                                  'printf("[b36dbg] FAIL 103AF8\\n"); '
                                  'fflush(stdout); } }\n'), 1)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
