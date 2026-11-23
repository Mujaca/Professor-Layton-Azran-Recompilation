p = 'build/gen_boot.c'
s = open(p, encoding='utf-8', errors='replace').read()

key = 'void fn_0010F8C4(ArmCpu *c) {'
if '/* gate3-stub */' in s:
    print('already')
elif key in s:
    i = s.find(key)
    j = s.find('\nvoid fn_', i + len(key))
    if j < 0:
        j = i + 400
    seg = key + '\n  /* gate3-stub */\n  c->r[0] = 1u; return;\n}\n'
    s = s[:i] + seg + s[j:]
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('patched')
else:
    print('missing')
