p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'gate3' not in s.lower():
    s += (
        '\n### gate3-Stub: Menue-Init laeuft jetzt erfolgreich\n\n'
        'Der Loader `fn_001036B4` scheiterte am dritten Check '
        '`fn_0010F8C4` (Unterpruefungen `0x11A890`/`0x11B05C`). '
        '`fn_0010F8C4` wurde als `mov r0,#1`-Stub gepatcht. Messung '
        '(`out/logs/g3_log.txt`):\n\n'
        '```\n'
        '[b36dbg] gate1 r0=1\n'
        '[b36dbg] gate2 r0=1\n'
        '[b36dbg3] gate3 r0=1\n'
        '[b36dbg3] gate4 r0=1\n'
        '[loader] loader0..4 r0=1\n'
        '[initdbg] success\n'
        '```\n\n'
        '`fn_00100F98` nimmt also jetzt den Erfolgszweig und laeuft in '
        'die Menue-Schleife. `menu+0x38/+0x6C` bleiben aber weiterhin '
        'leer (Write-Watch `3EE164`: nur BSS/Ctor), und es gibt keine '
        'Engine-`SetCommandList` (192, alle `lr=001ED9C8`), Pixel 0/0.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
