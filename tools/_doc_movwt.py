p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'MOVW/MOVT-Scan' not in s:
    s += (
        '\n**MOVW/MOVT-Scan:** `tools/_movwt.py` sucht nach gepaarten '
        'MOVW/MOVT, die eine Adresse in `0x28FF90..0x290050` bilden. '
        'Ergebnis: **0 Kandidaten** (beide Rd gleich, Abstand <= 80 B). '
        'Die Menue-Namenstabelle wird also weder per Literal noch per '
        'MOVW/MOVT adressiert; sie ist vermutlich reine Debug-/'
        'Animations-Benennung oder wird ueber eine Laufzeit-Basis '
        '(Zeiger in .data) erreicht.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
