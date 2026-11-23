p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'fn_00284D24-Gatekette' not in s:
    body = """

### fn_00284D24-Gatekette vermessen

`fn_00284D24` hat mehrere Ausstiegspunkte. Mit Instrumentierung
(`[resforce]`) und Watches auf die Blockadressen:

1. **Erstes Gate:** `bl fn_0011B98C` (0x284D60) muss != 0 liefern.
   `fn_0011B98C` -> `fn_00123780` -> `fn_0012BC58` (Rueckgabe r6 aus
   `0x289AA0`/`0x289B48`). Im Port ist das 0 ->
   `beq 0x285188` (16 Treffer).
2. Erzwingt man `fn_0011B98C -> 1`, ist `0x285188` = 0; der naechste
   Ausstieg ist `0x285194` (Gate bei `0x285048`:
   `[struct+0x4F8]` und `[struct+0x35C+0x1A4]` muessen != 0 sein).
3. Danach folgt der virtuelle Aufruf `[world]->vtable+0xA0`
   (0x285060); liefert er 0, geht es nach `0x285184` (36 Treffer).
4. Erst danach kommen `fn_0011E524` (0x285088) und der GX-Submit
   `fn_0011E494` (0x2850A8).

Der Init-Stack-Struct wird inzwischen mit 0x800 Byte nach
`0x0CFE9800` kopiert und `+0x4F8/+0x500` werden auf 1 gesetzt.
Trotzdem bleibt `ARM_WATCH=11E494` = 0.

Damit ist die Gatekette bekannt: Ressourcen-Bind (0x289AA0) ->
Struct-Felder (+0x4F8/+0x500) decimal -> virtueller Prep-Aufruf ->
`fn_0011E524` -> GX-Submit.
"""
    s += body.replace('decimal ->', '->')
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
