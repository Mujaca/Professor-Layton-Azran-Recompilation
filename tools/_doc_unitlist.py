p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Engine-Draw-Liste erreicht Count 1' not in s:
    body = """

### Engine-Draw-Liste erreicht Count 1, Engine sendet cmd 0x0006 selbst

Mit `HLE_ENGINE_MENU=1 HLE_MENU_OBJ0=1 HLE_DRAW_LIST=1
HLE_UNIT_XFER=1` (ohne HLE_ENGINE_3D) sind erstmals alle drei
Engine-Stufen gleichzeitig aktiv (`out/logs/ufull2_log.txt`):

```
[port] units=1 dl=1/256 arr=0CFF0000 e0=0CFF2000
[unitxfer] obj=0CFF2000 -> units=1, 3D-Renderer aufgerufen
[gsp] SetCommandList addr=0x00000001 size=0x0 lr=0034AFC4
[gsp] SetCommandList addr=0x00000001 size=0x0 lr=0034AFC4
[gsp] SetBufferSwap screen=0 fb=0x1F000000 ...
[gsp] SetBufferSwap screen=1 fb=0x1F060000 ...
```

Damit gilt erstmals:
- die Engine-Draw-Liste `menu+0x6C/+0x74` ist **nicht mehr 0** (`dl=1/256`),
- `units=1` (3D-Renderer `fn_001B9938`),
- zwei `SetCommandList` kommen aus **Engine-Code** (`lr=0034AFC4`,
  Aufrufer `fn_001B9938` bei `0x1BA3A0` -> `fn_0034AFAC`),
- `SetBufferSwap screen=0/1` wird gesetzt.

`addr=1/size=0` ist bei cmd `0x0006` aber korrekt: Azahar
(`tools/azahar/src/core/hle/service/gsp/gsp_gpu.cpp:439`) behandelt
`GSP_GPU::SetCommandList` als **Stub ohne Parameter**. Die echte
PICA-Liste laeuft ueber den GSP-Shared-Memory-CommandBuffer
(`SubmitCmdList`) + `TriggerCmdReqQueue` (`0x000C`).

Im Port ist dieser Ring aber leer: der Trigger-Log zeigt
`qops=0`, und im Bereich `0x10000000..0x10004000` gibt es keine
Gast-Schreibzugriffe. Der Engine-Renderer schreibt seine
`SubmitCmdList`-Queue also noch nicht (oder an eine andere Adresse).

**Naechster Schritt:** Den Engine-Schreiber der GSP-CommandBuffer-
Queue (`SubmitCmdList`-Eintrag) finden und dafuer sorgen, dass
`TriggerCmdReqQueue` ihn mit `qops>0` abarbeitet. Dann rendert die
Engine ihre PICA-Liste selbst; `addr=1/size=0` aus cmd 0x0006 ist
dafuer irrelevant.
"""
    s += body
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
