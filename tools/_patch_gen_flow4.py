p = 'build/gen_boot.c'
s = open(p, encoding='utf-8').read()
needle = ("  arm_dp(c, DP_MOV, 4, 0, c->r[0], 1, -1);\n"
          "  if (arm_cond(c, 14)) {   /* Asset-Registrierung erzwingen */\n"
          "    goto L_00100F98_001013F0;\n"
          "  }\n"
          "  goto L_00100F98_00101374;")
i = s.rfind(needle)
if i < 0:
    print('needle missing')
else:
    rep = ("  { static int fd = 0; if (fd < 4) { fd++; "
           "printf(\"[flowdbg] 2. jump-stelle r0=%08X abort=%d last=%08X\\n\", "
           "c->r[0], c->aborted, arm_last_block); fflush(stdout); } }\n" + needle)
    s = s[:i] + rep + s[i + len(needle):]
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok at', i)
