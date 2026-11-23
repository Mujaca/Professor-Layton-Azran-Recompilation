p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if '0x265DD0 ist ein Clear/' not in s:
    s += (
        '\n**`0x265DD0` ist ein Clear-/Teardown-Helfer**, kein Container-'
        'Get: `ldr r1,[r0]; ...; str r1,[r0]; strh r1,[r0,#0xa]; '
        'strh r1,[r0,#8]; bx lr` — er nullt `[r0]`, `[r0+8]`, `[r0+0xa]` '
        'und laesst `r0` unveraendert. Die Kette `bl 0x265DD0; sub '
        'r0,r0,#0xc; ...` im Aufrufer `0x3610xx` ist damit eher ein '
        'Abbau/Pop einer Member-Kette als ein Aufbau. Die eigentliche '
        'Menue-Konstruktion liegt also woanders.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
