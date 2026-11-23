p = 'build/gen_boot.c'
s = open(p, encoding='utf-8').read()
i = s.find('L_00100F98_0010123C:')
j = s.find('L_00100F98_00101374:', i)
seg = s[i:j]
marks = [
    ("  if (c->aborted) return;", "after-abort-checks"),
    ("  arm_dp(c, DP_MOV, 1, 0, c->r[4], 0, -1);", "after-stm"),
    ("  { uint32_t sp0 = c->r[13];\n    c->r[14] = 0x00101368u;", "before-1034F4"),
]
n = 0
for needle, msg in marks:
    k = seg.find(needle)
    if k >= 0:
        ins = ('  { static int fd = 0; if (fd < 4) { fd++; printf("[flowpt] ' + msg +
               ' pc=%08X abort=%d\\n", arm_last_block, c->aborted); fflush(stdout); } }\n')
        seg = seg[:k] + ins + seg[k:]
        n += 1
s = s[:i] + seg + s[j:]
open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('inserted', n)
