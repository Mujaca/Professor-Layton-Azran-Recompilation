"""Stellt den Unit-Zweig wieder her und beendet den GPU-Wait nach dem
ersten Unit-Durchlauf (Flag [0x3D8E6A] = 1)."""
p = 'build/gen_boot.c'
s = open(p, encoding='utf-8', errors='replace').read()

key = ('L_001B9938_001BA898:\n'
       '  arm_trace_block(c, 0x001BA898u, 0u);\n')
i = s.find(key)
if i < 0:
    print('block 1BA898 missing')
else:
    j = s.find('\nL_001B9938_', i + len(key))
    if j < 0:
        j = i + 900
    seg = s[i:j]
    # erster goto L_...1BA8A8 (im if) -> zurueck nach  lob 1BA364
    old = 'goto L_001B9938_001BA8A8;'
    if 'goto L_001B9938_001BA364;' not in seg and old in seg:
        seg = seg.replace(old, 'goto L_001B9938_001BA364;', 1)
        s = s[:i] + seg + s[j:]
        open(p, 'w', encoding='utf-8', newline='\n').write(s)
        print('branch restored')
    else:
        print('branch already/odd')

key2 = ('L_001B9938_001BA634:\n'
        '  arm_trace_block(c, 0x001BA634u, 0u);\n')
i2 = s.find(key2)
if i2 < 0:
    print('block 1BA634 missing')
elif 'gpu_wait_done' in s:
    print('flag already')
else:
    code = ('  {\n'
            '    static int gpu_wait_done = 0;\n'
            '    if (!gpu_wait_done) {\n'
            '      gpu_wait_done = 1;\n'
            '      arm_mem_write(c, 0x003D8E6Au, 1, 1);\n'
            '    }\n'
            '  }\n')
    s = s.replace(key2, key2 + code, 1)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('flag inserted')
