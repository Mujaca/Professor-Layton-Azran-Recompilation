p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Submodul-Matrix' not in s:
    s += (
        '\n**Submodul-Matrix:** Die vier Frame-Callback-Submodule direkt '
        'aufgerufen (`tools/_sub_matrix.py`):\n\n'
        '```\n'
        '28E40C(3EC028) files=4 unk=23 dl=0\n'
        '11AF00(3EC028) files=4 unk=40 dl=0\n'
        '11B02C(3EC028) files=4 unk=40 dl=0\n'
        '121F3C(3EC5C4) files=4 unk=32 dl=0\n'
        '```\n\n'
        '`files=4` sind nur die `gamecoin.dat`-Oeffnungen; keine Menue-'
        'Assets, `menu+0x74` bleibt 0, keine Engine-`SetCommandList`.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
