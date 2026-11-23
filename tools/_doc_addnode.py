p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Add-Node-Matrix' not in s:
    s += (
        '\n**Add-Node-Matrix:** Kandidaten aus dem Menue-Code (Funktionen, '
        'die `+0x74` und `+0x6C` nutzen) per `HLE_FRAME_LOOP2` mit '
        '`r0=0x3EE12C, r1=0x0CFF2100` aufgerufen (`tools/_addnode_matrix.py`):\n\n'
        '```\n'
        '2721E0 fl=22 unk=25 dl=0/0\n'
        '2835EC fl=22 unk=36 dl=0/0\n'
        '286094 fl=22 unk=57 dl=0/0\n'
        '286220 fl=1  unk=200 dl=0/0\n'
        '289428 fl=22 unk=67 dl=0/0\n'
        '28DE80 fl=21 unk=48 dl=0/0\n'
        '28E02C fl=22 unk=40 dl=0/0\n'
        '```\n\n'
        'Keine dieser Funktionen erhoeht `menu+0x74` (`dl` bleibt 0/0); '
        'der Add-Node-Pfad braucht weitere Argumente/Zustand.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
