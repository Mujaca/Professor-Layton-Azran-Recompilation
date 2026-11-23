p = 'build/gen_boot.c'
s = open(p, encoding='utf-8').read()
old = '  c->r[0] = 0u;   /* Diagnose: 2. fn_001034F4 blockiert (Menue-Loop) */'
new = '  c->r[0] = 1u;   /* Diagnose: 2. fn_001034F4 blockiert (Menue-Loop), Erfolg */'
n = s.count(old)
s = s.replace(old, new)
open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('replaced', n)
