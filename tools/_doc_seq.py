p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Menue-Namenstabelle' not in s:
    s += (
        '\n### Menue-Namenstabelle gefunden (0x28FFDC..)\n\n'
        'In der Exe stehen die Menue-Sequenznamen als Inline-Strings:\n\n'
        '```\n'
        '0x28FFDC "menu_logo"\n'
        '0x28FFF4 "menu_title"\n'
        '0x290000 "menu_name"\n'
        '0x29000C "menu_finish"\n'
        '0x290024 "menu_bag", "menu_invest", "menu_fukamaru", "menu_jiten" ...\n'
        '```\n\n'
        'Davor/dazwischen stehen Code-Zeiger (`0x104420`, `0x104428`, '
        '`0x104440`, `0x1045E0`, `0x1045E8`, `0x10470C`, `0x1047DC`, '
        '`0x1047E4`, ...) — offenbar eine Tabelle von Menue-Seiten mit '
        'Handlern und Sequenznamen.\n\n'
        '**Test:** Diese Handler (`10470C/1047DC/1047E4/104420/104428/'
        '104440`) per `HLE_FRAME_LOOP2` mit `r0=0x3EE12C` aufgerufen '
        '(`tools/_seq_matrix.py`): keine `.xq`-Datei wird geoeffnet, '
        '`menu+0x74` bleibt 0. Der eigentliche Sequenz-Loader ist also '
        'nicht darunter.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
