p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Scene-Klassen-Vtable' not in s:
    s += (
        '\n### Scene-Klassen-Vtable @ 0x3CA370 gefunden\n\n'
        'Der Literal-Scan nach `0x0035F3B4` findet die Funktion in einer '
        'statischen Tabelle:\n\n'
        '```\n'
        '0x3CA370 00000000\n'
        '0x3CA374 0035F3B4   <- Scene-Builder (Methode +0x04)\n'
        '0x3CA37C 0010F0B4\n'
        '0x3CA380 0036C6E8\n'
        '0x3CA388 0036C54C\n'
        '0x3CA38C 0036C60C\n'
        '0x3CA394 0036CBD0\n'
        '0x3CA3A8 00273908\n'
        '0x3CA3AC 0037EF94\n'
        '```\n\n'
        'Damit ist klar, wie der Builder aufgerufen wird: als **Vtable-'
        'Methode bei +0x04** eines Objekts, dessen `[0] = 0x3CA370` ist. '
        'Im Port zeigt die Scene (`0x3CA494`) aber `[0]=0x003653CC` — '
        'also eine andere/aeltere Klasse; ein Aufruf von +0x04 der '
        '0x3CA370-Tabelle wurde nie beobachtet (`ARM_WATCH=3610F8`: 0).\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
