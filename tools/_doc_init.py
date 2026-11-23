p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Menue-Init nimmt den Fail-Zweig' not in s:
    s += (
        '\n### Menue-Init nimmt den Fail-Zweig (gemessen)\n\n'
        'Instrumentierung in `fn_00100F98` (`build/gen_boot.c`, initdbg/'
        'loader-Prints):\n\n'
        '```\n'
        '[loader] loader0 r0=0\n'
        '[initdbg] fail\n'
        '```\n\n'
        '`fn_00100F98` springt also bei `0x101030` in den Fail-Pfad '
        '`0x1014E4`, weil der erste Loader `fn_001036B4` 0 liefert. '
        'Dessen erster Check ist `fn_0010FC9C` (liest `*(s8*)0x3D8CE0`); '
        'ein dauerhaftes Setzen von `[0x3D8CE0]=1]` (HLE_PUMP bei '
        '`HLE_ENGINE_MENU`) aendert nichts — der Fehlschlag liegt also '
        'tiefer in `0x1036B4` (`0x1036C8`, `0x103724` oder Folgeschritte).\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
