p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'GSP-Client-Init wird im Port nie aufgerufen' not in s:
    body = """

### GSP-Client-Init wird im Port nie aufgerufen - Queue-Basis fehlt

`fn_00138D70` (GSP-Client-Init) hat im recompilierten Code **keinen
Aufrufer** (kein `fn_00138D70(c)` in `build/gen_boot.c`, kein BL im
gesamten Text). Im Normallauf laeuft sie also nie; bisher wurde sie
nur ueber `HLE_GSP_ENGINE` manuell angestossen.

Mit `HLE_GSP_ENGINE=1` laeuft sie und legt den Client-Kontext an
(`out/logs/ctx_log.txt`):

```
[gspengine] ctx=003D7C40 +38=00000000 ... +40=00000000 ...
            +58=00000800 +5C=00000200 +60=00000240
            +64=FFFFFFFF +68=00000001
[gspengine] 0x138D70: h10=4 h14=4 state=003D7C40 -> gsp=2
```

Bedeutung:
- `ctx+0x40` = GSP-Shared-Memory-Basis = **0**,
- `ctx+0x58` = CommandBuffer = **0x800** (Basis+0x800, also bei 0),
- `ctx+0x5C/+0x60` = 0x200/0x240 (Basis+0x200/+0x240).

Der Map-Pfad ist `0x13EAAC` -> `0x11F2C8` -> `svc 0x1F` mit
`mov r3,#0x10000000`; die Adresse landet aber nicht in `ctx+0x40`.

Setzt man per `HLE_GSP_BASE=1` die erwartete Basis:

```
[gspengine] ... base=10002000 q58=10002800
```

dann bleibt `qops=0` und die Engine schreibt dennoch keinen
`SubmitCmdList`-Eintrag (192 HLE-Listen + 2 Engine-Stubs
`lr=0034AFC4`). Es fehlt also nicht nur die Basis, sondern auch der
Aufruf des Submit-Schreibers, der `ctx+0x58` benutzt.
"""
    s += body
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
