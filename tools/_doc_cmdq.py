p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Kein SubmitCmdList-Puffer im Gast-RAM' not in s:
    body = """

### Kein SubmitCmdList-Puffer im Gast-RAM gefunden

Ein neuer Scanner (`HLE_SCAN_CMDQ`) sucht im gesamten Gast-RAM
`0x08000000..0x20000000` (0x200-Alignment) nach einem
GSP-CommandBuffer mit gueltigem SubmitCmdList-Eintrag
(`id==1`, `addr` im RAM, `size` 1..0x100000). Ergebnis im
Komplettlauf (`HLE_ENGINE_MENU=1 HLE_MENU_OBJ0=1 HLE_DRAW_LIST=1
HLE_UNIT_XFER=1`, `out/logs/cmdq2_log.txt`):

```
[cmdq] scan:
[port] units=1 dl=1/256 arr=0CFF0000 e0=0CFF2000
```

Also **0 Treffer**: Die Engine schreibt ihre PICA-Liste in keinen
GSP-CommandBuffer. Der Trigger bleibt bei `qops=0`.

### 3D-Unit-Node und Engine-Objekt (Dump)

Mit `HLE_MENU_DUMP=1` (`out/logs/ud2_log.txt`):

```
[menuobj] arr=0CFF0000 cnt=1: [0]=0CFF2000
[menuobj] obj0CFF2000: 00000000 00000000 00000002 00000000 000003E8 ...
[menuobj] unitnode: 0CFF2000 00000000 00000000 00000000 0CFF2000 0CFF2000 ...
[menuobj] units=1 u0=0CFF2000: ... 00030010 00103FA0 00104088 00104090 003EC028
```

Der Builder erzeugt also ein Objekt, das die Klassendescriptor-Worte
`0x103FA0/0x104088/0x104090` und den Weltzeiger `0x3EC028` traegt,
aber **keine Vtable** (`[obj]==0`).

### GSP-Kommando-Zensus des Engine-Laufs

`out/logs/ufull2_log.txt` (Auszug):

```
1979x gsp::Gpu cmd=0x0007 n=4   (RequestDma, Parameter p0=0 p1=1 p2=0)
 192x gsp::Gpu cmd=0x0006 n=2   (HLE-2D-Liste 0x0CFE6000/0x24)
 192x gsp::Gpu cmd=0x000C n=0   (Trigger, qops=0)
   2x gsp::Gpu cmd=0x0006 n=1   (Engine-Stub, lr=0034AFC4, addr=1/size=0)
```

Die Engine benutzt also fast nur `RequestDma` (degeneriert) und den
`SetCommandList`-Stub; der CommandBuffer-Schreiber fehlt.
"""
    s += body
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
