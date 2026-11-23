p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Stack-Areal' not in s:
    s += (
        '\n**Stack-Areal getestet:** Builder-Aufruf mit eigenem grossen '
        'Stack (`SP=0x0D840000`) aendert nichts: weiterhin 200x '
        '`unbekanntes call-Ziel`, davon `0xE5900008` (Block `0x28EF94`). '
        '`0xE5900008` ist eine ARM-Instruktion (`ldr r0,[r0,#8]`), d. h. '
        'ein Feld des Scene-Objekts wurde als **Vtable-/Funktionszeiger** '
        'interpretiert und zeigte in den Code. Die vom Forced-Ctor '
        '(`0x381CE4`) gebaute Scene hat also eine andere/partielle '
        'Layout-Ordnung als die erwartete Klassen-Struktur.\n'
    )
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
