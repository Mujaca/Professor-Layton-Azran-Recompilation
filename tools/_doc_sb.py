p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
s = s.replace(
    '**Aufrufversuch `HLE_SCENEBUILD=1`:**',
    '**Echter Einstieg gefunden:** Push-Prolog-Scan (`tools/_findfn.py`) '
    'zeigt, dass der Builder bei **0x0035F3B4** beginnt (nicht 0x35FF14; '
    '0x35FF14 liegt mitten im Code).\n\n'
    '**Aufrufversuch `HLE_SCENEBUILD=1`:**')
s = s.replace('`0x3CA494` bzw. `0x3EC028`', '`0x3CA494`, `0x3EC028` bzw. `0x3EE12C`')
open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
