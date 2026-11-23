p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Unit-Node +0x18 speist RequestDma' not in s:
    body = """

### Unit-Node +0x18 speist RequestDma (Feldzuordnung belegt)

Mit `HLE_UNIT_XFER` und `node+0x18 = 0x0CFF4000`
(`runtime/hle/hle.c`) zeigt der GSP-Log die Feldzuordnung:

```
[gsp] RequestDma p0=0CFF4000 p1=00000001 p2=00000000 p3=00000000
[gsp] RequestDma p0=00000000 p1=00000001 p2=00000000 p3=00000000
```

`p0` ist also exakt `[node+0x18]`; `p1` kommt aus der Klassentabelle
(`[sb+typ]` = 1), `p2/p3` bleiben 0. Die DMA hat damit `size=0` und
kopiert nichts. Die HLE-`RequestDma`-Implementierung kopiert jetzt
bei plausiblen Parametern (`src`, `dst`, `size`, `flags`), logt aber
weiterhin `qops=0` beim Trigger.

Damit ist belegt: der Unit-Node ist die richtige Stelle, aber
`+0x18` allein reicht nicht; die uebrigen DMA-Felder (Ziel und
Groesse) stammen aus einer anderen Quelle und sind im Port 1/0.
"""
    s += body
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
