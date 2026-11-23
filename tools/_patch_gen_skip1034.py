p = 'build/gen_boot.c'
s = open(p, encoding='utf-8').read()
needle = ("  { uint32_t sp0 = c->r[13];\n"
          "    c->r[14] = 0x00101368u;\n"
          "    fn_001034F4(c);\n"
          "    arm_restore_sp(c, sp0); }")
if needle in s:
    rep = ('  { static int sk = 0; if (sk < 4) { sk++; '
           'printf("[skip1034] 2. fn_001034F4 uebersprungen\\n"); fflush(stdout); } }\n'
           '  c->r[0] = 0u;   /* Diagnose: 2. fn_001034F4 blockiert (Menue-Loop) */')
    s = s.replace(needle, rep, 1)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('patched')
else:
    print('missing')
