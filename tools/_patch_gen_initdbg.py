p = 'build/gen_boot.c'
s = open(p, encoding='utf-8', errors='replace').read()

if 'initdbg' in s:
    print('already')
else:
    for addr, tag in [('00101034', 'success'), ('001014E4', 'fail'),
                      ('001013F0', 'asset-block')]:
        key = ('L_00100F98_' + addr + ':\n'
               '  arm_trace_block(c, 0x' + addr + 'u, 0u);\n')
        if key not in s:
            print('missing', addr)
            continue
        code = ('  { static int id = 0; if (id < 4) { id++; '
                'printf("[initdbg] ' + tag + '\\n"); fflush(stdout); } }\n')
        s = s.replace(key, key + code, 1)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
