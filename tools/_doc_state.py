p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'State-Methoden-Matrix' not in s:
    s += (
        '\n**State-Methoden-Matrix:** Die Methoden der State-Tabelle '
        '`0x3C6FE4` (`0x37F948`, `0x37F910`, `0x376F98`, `0x37F9BC`, '
        '`0x376F70`, `0x37FBC8`, `0x298730`, `0x29880C`) wurden per '
        '`HLE_FRAME_LOOP2` mit `r0=0x3C6FE4` aufgerufen '
        '(`tools/_state_matrix.py`): alle laden nur `gamecoin.dat` '
        '(4 Oeffnungen), `menu+0x74` bleibt 0, keine Engine-'
        '`SetCommandList`. `0x29880C` laeuft ohne Null-Calls, hat aber '
        'keine Wirkung auf die Menue-Listen.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
