"""Pfad-Diagnose in fn_001B9938: markiert erreichte Bloecke."""
p = 'build/gen_boot.c'
s = open(p, encoding='utf-8', errors='replace').read()

BLOCKS = ['001B99D0', '001BA01C', '001BA034', '001BA130', '001BA180',
          '001BA294', '001BA2FC', '001BA310', '001BA328', '001BA364']

if 'pathdbg' in s:
    print('already')
else:
    missing = []
    for addr in BLOCKS:
        key = ('L_001B9938_' + addr + ':\n  arm_trace_block(c, 0x' +
               addr + 'u, 0u);\n')
        if key not in s:
            missing.append(addr)
            continue
        code = (
            '  {\n'
            '    static int pd = 0;\n'
            '    if (pd < 4) {\n'
            '      pd++;\n'
            '      printf("[pathdbg] block=' + addr + ' r0=%u\\n",\n'
            '             (unsigned)c->r[0]);\n'
            '      fflush(stdout);\n'
            '    }\n'
            '  }\n')
        s = s.replace(key, key + code, 1)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('written; missing', missing)
