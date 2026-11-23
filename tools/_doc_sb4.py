p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Frisches Scene-Objekt' not in s:
    s += (
        '\n**Frisches Scene-Objekt getestet:** `HLE_SB_NEW=1` legt ein '
        'genulltes 0x4000-Byte-Objekt bei `0x0D200000` an, setzt die '
        'Klassen-Vtable `0x3CA370` und ruft den Builder. Ergebnis '
        '(`out/logs/sbnew_log.txt`): weiterhin 200x '
        '`unbekanntes call-Ziel` (jetzt 0x0), keine '
        'Engine-`SetCommandList`, `units=0`, `menu`-Listen leer. Der '
        'Builder setzt also bereits initialisierte Sub-Objekte voraus.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
