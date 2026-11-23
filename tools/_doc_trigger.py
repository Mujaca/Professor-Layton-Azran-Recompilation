p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'GSP-Sender-Kette und fehlender Trigger' not in s:
    body = """

### GSP-Sender-Kette und fehlender Trigger

Die RequestDma-Aufrufe (`cmd=0x0007 n=4`, p0=0/p1=1) kommen aus dem
3D-Renderer selbst: `fn_001B9938` ruft bei `0x1BA3DC`, `0x1BA5A0`
und `0x1BA624` den generischen Sender `0x24B258` -> `0x34B55C`
(Header `0x00070102`, normal=4). Die Parameter stammen aus den
Unit-Node-Feldern:

```
r0 = [node+0x10]        r1 = [node + idx*4 + 0x18]
r2 = klasse_tabelle[typ]  r3 = [[sp+0xd4]]
```

Unser synthetischer Node (`HLE_UNIT_XFER`) setzt nur `+0x10/+0x14`,
`+0x18` bleibt 0 — daher die degenerierten Parameter.

Die eigentlichen Trigger-Wrapper (`cmd 0x000C`, normal=1) sind:
`0x13E3E0`, `0x3418BC`, `0x341D34` (`out/logs/tr*.txt`).
`ARM_WATCH` auf alle drei im Komplettlauf: **0 Treffer** — die Engine
ruft ihren `TriggerCmdReqQueue` nie auf.

Am Renderer-Ende (`0x1BA954`) wird nur ein GSP-Handle geschlossen:
`ldr r0,[0x3DA6DC]; bl 0x34ABB4` -> `0x34B690` sendet `cmd 0x003A`.
"""
    s += body
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
