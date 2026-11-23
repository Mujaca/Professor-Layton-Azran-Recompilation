p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'strh-#0x74-Scan' not in s:
    s += (
        '\n**strh-#0x74-Scan:** `tools/_find_strh74.py` (Capstone, jede '
        '4-Byte-ARM-Instruktion) findet **0** direkte '
        '`strh rX,[rY,#0x74]`. Der Node-Count wird also ueber eine '
        'berechnete Adresse geschrieben (kein direkter Offset), was die '
        'statische Suche nach dem Add-Node weiter erschwert.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
