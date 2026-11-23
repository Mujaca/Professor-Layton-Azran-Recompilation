p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Welt-Update ruft den Drain' not in s:
    s += (
        '\n**Welt-Update ruft den Drain:** `callgraph callers 0x11DB84` '
        'zeigt: `fn_00285208` (Welt-Update) ruft bei `0x285418` die '
        'Menue-Drain-Methode `0x11DB84` -> `0x277004`. Die Kette ist also '
        '`fn_00100F98 -> fn_001034F4 -> fn_00108F70 -> fn_00285208 -> '
        '0x11DB84 -> 0x277004`; der Welt-Update ist damit die Stelle, an '
        'der Menue-Nodes entstehen muessten (er drainiert sie direkt '
        'danach). Bei unseren direkten Aufrufen von `fn_00285208` kamen '
        'jedoch Null-Calls (`0x28EF94`).\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
