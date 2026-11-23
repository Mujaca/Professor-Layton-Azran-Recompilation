p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Vtable-Test' not in s:
    s += (
        '\n**Vtable-Test:** `HLE_SCENEBUILD=1 HLE_SB_ARG=3CA494` setzt '
        'zusaetzlich `[scene]=0x3CA370` (Klassen-Vtable) und ruft den '
        'Builder. Ergebnis (`out/logs/vt2_log.txt`): weiterhin 200x '
        '`unbekanntes call-Ziel` (0xE5900008/0), keine '
        'Engine-`SetCommandList` (192, alle `lr=001ED9C8`), `units=0`. '
        'Der Builder braucht also weitere initialisierte Sub-Objekte, '
        'nicht nur die Vtable.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
