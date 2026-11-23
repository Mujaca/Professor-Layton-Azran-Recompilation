p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Es werden keine Menue-Sequenzen geladen' not in s:
    s += (
        '\n**Es werden keine Menue-Sequenzen geladen:** Im Lauf mit '
        'erfolgreichem Init (`out/logs/xq_log.txt`) sind die einzigen '
        'Datei-Requests 4x `/gamecoin.dat` (jeweils 0 KiB). Kein `.xq` '
        '(menu_title/menu_logo/...) und kein `.xi` wird angefordert. Die '
        'Menue-Inhalte (Sequenzen/Assets) werden also gar nicht erst '
        'geladen -> keine Nodes. Das erklaert die leeren Listen trotz '
        'erfolgreichem Init.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
