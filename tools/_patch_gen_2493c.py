p = 'build/gen_boot.c'
s = open(p, encoding='utf-8').read()
sites = [
    ('0x001249B0u', 'fn_00284AA8(c);'),
    ('0x001249C4u', 'fn_001300C8(c);'),
    ('0x001249E8u', 'fn_00284AA8(c);'),
    ('0x001249FCu', 'fn_00130170(c);'),
]
n = 0
for lr, fn in sites:
    old = ('    c->r[14] = %s;\n    %s\n    arm_restore_sp(c, sp0); }\n'
           '  if (c->aborted) return;' % (lr, fn))
    if old in s:
        new = old + '\n  c->r[0] = 1u;   /* Gate in fn_0012493C erzwingen */'
        s = s.replace(old, new, 1)
        n += 1
    else:
        print('missing', lr)
open(p, 'w', encoding='utf-8', newline='\n').write(s)
printInc = 0
print('patched', n)
