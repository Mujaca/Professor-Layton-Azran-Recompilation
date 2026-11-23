p = 'build/gen_boot.c'
s = open(p, encoding='utf-8').read()
i = s.find('L_00100F98_0010123C:')
j = s.find('L_00100F98_00101374:', i)
if i < 0 or j < 0:
    print('range missing')
else:
    seg = s[i:j]
    marker = "  if (c->aborted) return;"
    cnt = seg.count(marker)
    seg = seg.replace(marker,
        "  { if (c->aborted) { static int fa = 0; if (fa < 6) { fa++; "
        "printf(\"[flowabort] r14=%08X last=%08X\\n\", c->r[14], arm_last_block); "
        "fflush(stdout); } } }\n" + marker)
    s = s[:i] + seg + s[j:]
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('patched', cnt)
