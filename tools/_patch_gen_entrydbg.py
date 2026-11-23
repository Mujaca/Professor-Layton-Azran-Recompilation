"""Diagnose am Einstieg und am Early-Exit von fn_001B9938."""
p = 'build/gen_boot.c'
s = open(p, encoding='utf-8', errors='replace').read()

if 'entrydbg' in s:
    print('already')
else:
    key = 'void fn_001B9938(ArmCpu *c) {'
    code = key + '''
  {
    static int ed = 0;
    if (ed < 8) {
      uint32_t en = (unsigned)arm_ld(c, c->r[0] + 0x15A3u, 1, 0);
      ed++;
      printf("[entrydbg] r0=%08X en=%u\\n", c->r[0], en);
      fflush(stdout);
    }
  }'''
    if key in s:
        s = s.replace(key, code, 1)
        key2 = ('L_001B9938_001BA964:\n'
                '  arm_trace_block(c, 0x001BA964u, 0u);\n')
        if key2 in s:
            s = s.replace(key2, key2 + '''  {
    static int xd = 0;
    if (xd < 8) { xd++; printf("[entrydbg] early-exit 1BA964\\n"); fflush(stdout); }
  }
''', 1)
        open(p, 'w', encoding='utf-8', newline='\n').write(s)
        print('written')
    else:
        print('fn not found')
