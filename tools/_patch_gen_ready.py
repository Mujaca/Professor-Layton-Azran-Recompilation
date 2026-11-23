import os

p = 'build/gen_boot.c'
s = open(p, encoding='utf-8').read()
key = 'void fn_0037EF94(ArmCpu *c) {'
i = s.find(key)
if i < 0:
    print('fn not found')
elif '/* ready-stub */' in s[i:i + 400]:
    print('already')
else:
    j = s.find('\nvoid fn_', i + len(key))
    if j < 0:
        j = len(s)
    seg = key + '\n  /* ready-stub */\n  c->r[0] = 0u; return;\n}\n'
    s = s[:i] + seg + s[j:]
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('patched')
