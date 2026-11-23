"""Flow-Diagnose in fn_001B9938 an den Bloecken 0x1BA364/438/454."""
p = 'build/gen_boot.c'
s = open(p, encoding='utf-8', errors='replace').read()

TEMPLATE = (
    '  {\n'
    '    uint32_t fcnt = arm_ld(c, 0x00423730u, 4);\n'
    '    static int fd = 0;\n'
    '    if (fd < 8) {\n'
    '      fd++;\n'
    '      printf("[flowdbg] block=TAG count=%u dc=%u\\n",\n'
    '             fcnt, (unsigned)arm_ld(c, c->r[13] + 0xDCu, 4, 0));\n'
    '      fflush(stdout);\n'
    '    }\n'
    '  }\n')

if 'flowdbg' in s:
    print('already')
else:
    ok = True

    def add(addr, tag):
        global s, ok
        key = ('L_001B9938_' + addr + ':\n  arm_trace_block(c, 0x' +
               addr + 'u, 0u);\n')
        if key not in s:
            print('missing', addr)
            ok = False
            return
        s = s.replace(key, key + TEMPLATE.replace('TAG', tag), 1)

    add('001BA364', '0x1BA364')
    add('001BA438', '0x1BA438')
    add('001BA454', '0x1BA454')
    if ok:
        open(p, 'w', encoding='utf-8', newline='\n').write(s)
        print('written')
