p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'ctx+0x58 ist der GSP-CommandBuffer' not in s:
    body = """

### ctx+0x58 ist der GSP-CommandBuffer (per Reset-Funktion belegt)

Die Init-Funktion ruft bei `0x13EBC4` `0x14250C(ctx+0x58, base+0x800+idx*0x200)`.
`0x14250C` ist ein **Reset des CommandBuffer-Headers**: es speichert
den Zeiger in `[ctx+0x58]` und schreibt das 32-Bit-Headerwort an
dieser Adresse mit Index/Number_commands/Status/Should_stop = 0 neu
(`ldrex/strex`-Schleife). Damit ist `ctx+0x58` eindeutig die
Azahar-`CommandBuffer`-Basis.

Der GX-Kommando-Builder liegt im Bereich `0x13F2xx` (Liste mit
0x48-Byte-Eintraegen, ruft `0x140178`, `0x142AF4`, `0x142AA8`).
`0x13F474` laeuft die Listen an `ctx+0x58`, `ctx+0x38`, `ctx+0x78`
ab und korrigiert Zeiger (Relocation).

Mit `HLE_GSP_BASE=1` wird `ctx+0x40=0x10002000` und
`ctx+0x58=0x10002800` gesetzt; der Reset `0x14250C` muss danach
laufen, damit der Header an `0x10002800` steht. Der Builder wird im
Port aber nicht aufgerufen, daher bleibt `qops=0`.

RequestDma-Feldzuordnung (Nachtrag): `p0 = [node+0x18]`,
`p1 = klasse_tabelle[typ]`, `p2=p3=0`; die HLE-DMA kopiert jetzt
bei plausiblen Parametern, im Port ist `size` aber 0.
"""
    s += body
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
