p = 'runtime/hle/hle.c'
s = open(p, encoding='utf-8').read()
s = s.replace('{ int snew = (getenv("HLE_SB_NEW") != NULL);',
              'int snew = (getenv("HLE_SB_NEW") != NULL);')
open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
