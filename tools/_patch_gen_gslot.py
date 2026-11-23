"""Setzt im Unit-Block 0x1BA390 den gsp-Handle in [0x3DA6DC]."""
p = 'build/gen_boot.c'
s = open(p, encoding='utf-8', errors='replace').read()

key = ('L_001B9938_001BA390:\n'
       '  arm_trace_block(c, 0x001BA390u, 0u);\n')

code = (
    '  {\n'
    '    extern uint32_t g_hle_gsp_export;\n'
    '    if (g_hle_gsp_export != 0u)\n'
    '      arm_mem_write(c, 0x003DA6DCu, 4, g_hle_gsp_export);\n'
    '  }\n')

if 'g_hle_gsp_export' in s:
    print('already')
elif key in s:
    s = s.replace(key, key + code, 1)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('patched')
else:
    print('block missing')
