p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Nach erfolgreichem Init' not in s:
    s += (
        '\n**Nach erfolgreichem Init:** Write-Watch auf `0x3EE198`/'
        '`0x3EE1A0` zeigt weiterhin nur BSS/Ctor (`out/logs/c1_log.txt`, '
        '`c2_log.txt`). Ein zusaetzlicher Welt-Update-Aufruf '
        '`0x10F0B4(0x3EC028)` (`HLE_FRAME_LOOP2`) endet wieder in '
        'Null-Calls (`0x28EF94`), `units=0`, keine Engine-`SetCommandList` '
        '(`out/logs/wu_log.txt`). Auch ein erfolgreicher Menue-Init legt '
        'im Port also keine Nodes an — die Inhalte kommen offenbar erst '
        'aus spaeteren Zustands-/Sequenzschritten (Eingaben/Events).\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
