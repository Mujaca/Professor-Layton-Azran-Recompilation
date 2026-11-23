"""Fuegt in fn_001B9938 am Unit-Loop-Block 0x1BA390 eine Diagnose ein."""
p = 'build/gen_boot.c'
s = open(p, encoding='utf-8', errors='replace').read()
key = '''L_001B9938_001BA390:
  arm_trace_block(c, 0x001BA390u, 0u);
'''
add = '''  {
    uint32_t ucnt = 0;
    static int udbg = 0;
    memcpy(&ucnt, c->mem + 0x00423730u, 4);
    if (udbg < 8) {
      udbg++;
      printf("[unitloop] count=%u node=%08X\\n", ucnt, c->r[4]);
      fflush(stdout);
    }
  }
'''
if 'unitloop' in s:
    print('already')
elif key in s:
    s = s.replace(key, key + add, 1)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('patched')
else:
    print('block not found')
