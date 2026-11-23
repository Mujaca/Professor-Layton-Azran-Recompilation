p = 'build/gen_boot.c'
s = open(p, encoding='utf-8').read()
needle = "  arm_dp(c, DP_MOV, 4, 0, c->r[0], 1, -1);\n  if (arm_cond(c, 14)) {   /* Asset-Registrierung erzwingen */"
if needle in s:
    s = s.replace(needle,
                  "  { static int fd = 0; if (fd < 4) { fd++; printf(\"[flowdbg] vor asset-jump r0=%08X abort=%d\\n\", c->r[0], c->aborted); fflush(stdout); } }\n"
                  "  arm_dp(c, DP_MOV, 4, 0, c->r[0], 1, -1);\n  if (arm_cond(c, 14)) {   /* Asset-Registrierung erzwingen */", 1)
needle2 = "L_00100F98_0010123C:\n  arm_trace_block(c, 0x0010123Cu, 0u);"
if needle2 in s:
    s = s.replace(needle2,
                  "L_00100F98_0010123C:\n  arm_trace_block(c, 0x0010123Cu, 0u);\n"
                  "  { static int fd = 0; if (fd < 4) { fd++; printf(\"[flowdbg] block 10123C abort=%d\\n\", c->aborted); fflush(stdout); } }", 1)
needle3 = "L_00100F98_001013F0:\n  arm_trace_block(c, 0x001013F0u, 0u);"
if needle3 in s:
    s = s.replace(needle3,
                  "L_00100F98_001013F0:\n  arm_trace_block(c, 0x001013F0u, 0u);\n"
                  "  { static int fd = 0; if (fd < 4) { fd++; printf(\"[flowdbg] block 1013F0 abort=%d\\n\", c->aborted); fflush(stdout); } }", 1)
open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
