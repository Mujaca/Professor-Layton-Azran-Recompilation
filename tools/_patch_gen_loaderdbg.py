p = 'build/gen_boot.c'
s = open(p, encoding='utf-8', errors='replace').read()

if 'loader[0]' in s:
    print('already')
else:
    pairs = [('001036B4', '0x001016E8u', 'loader0'),
             ('00103B48', '0x00101708u', 'loader1'),
             ('00103684', '0x00101718u', 'loader2'),
             ('001035D8', '0x00101728u', 'loader3'),
             ('00103610', '0x0010173Cu', 'loader4')]
    for fn, ret, tag in pairs:
        old = ('  { uint32_t sp0 = c->r[13];\n'
               '    c->r[14] = ' + ret + ';\n'
               '    fn_' + fn + '(c);\n'
               '    arm_restore_sp(c, sp0); }\n')
        if old not in s:
            print('missing', tag)
            continue
        add = old + ('  { static int ld=0; if (ld<5) { ld++; '
                     'printf("[loader] ' + tag + ' r0=%u\\n", '
                     '(unsigned)c->r[0]); fflush(stdout); } }\n')
        s = s.replace(old, add, 1)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    printoped = None
