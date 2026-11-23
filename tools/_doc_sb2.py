p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Live-Aufrufstelle' not in s:
    s += (
        '\n**Live-Aufrufstelle `0x3610F8`:** `ARM_WATCH=3610F8chang` im '
        'Menue-Loop -> 0 Treffer; der Builder wird vom Spiel im Port also '
        'gar nicht aufgerufen. Statisch gilt: `r4 = container(0x265DD0) - '
        '0x80`, also ist das Menue das Member bei `container+0x80`.\n'
        'Aufrufe von `fn_0035F3B4` mit `0x3EE1AC`/`0x3EE0AC` liefern '
        'weiterhin `unbekanntes call-Ziel` (200x); bei `0x3EE1AC` waechst '
        'immerhin der Pending-Pool (`0..1`, `mn=0`).\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    printoped = 'already'
