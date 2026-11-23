p = 'build/gen_boot.c'
s = open(p, encoding='utf-8').read()
needle = "  if (arm_ldm_stm(c, 8, 1, 0, 0, 0x00001080u)) return;"
i = s.find(needle, s.find('L_00100F98_0010123C:'))
if i < 0:
    print('missing')
else:
    rep = ("  { static int fd = 0; if (fd < 4) { fd++; "
           "printf(\"[ldmdbg] base8=%08X val7=%08X\\n\", c->r[8], c->r[7]); fflush(stdout); } }\n"
           + needle +
           "\n  { static int fd = 0; if (fd < 4) { fd++; "
           "printf(\"[ldmdbg] passed r7=%08X ip=%08X abort=%d\\n\", c->r[7], c->r[12], c->aborted); fflush(stdout); } }")
    s = s[:i] + rep + s[i + len(needle):]
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
