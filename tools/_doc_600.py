p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if '600-Frame-Lauf' not in s:
    s += (
        '\n**600-Frame-Lauf:** Auch nach 600 Frames (~20 s) bleibt '
        '`menu+0x74=0`, `units=0`, `pool=0..0`; nur 4 `gamecoin.dat`-'
        'Oeffnungen; alle `SetCommandList` (192) mit `lr=001ED9C8` '
        '(`out/logs/600_log.txt`). Der Menue-Loop laeuft, erzeugt aber '
        'keine Inhalte.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
