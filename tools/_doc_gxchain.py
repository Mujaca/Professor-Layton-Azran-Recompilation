p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'GX-Submit-Kette identifiziert' not in s:
    body = """

### GX-Submit-Kette identifiziert

Die Engine-Submission laeuft ueber diese Kette (Callgraph):

```
fn_00284D24  (0x109090 / 0x109514 aus fn_00108F70)
  -> fn_0011E494  (0x2850A8)
  -> fn_0012493C  (0x124A3C)
  -> fn_0012FC24  (GX-Submit, 0x12FDA8)
  -> fn_00139914  (Builder, 0x139948)
  -> fn_0013F2D4  (Queue-Eintrag, 0x13F2D4)
```

`fn_0013F2D4(r0, r1, r2, r3, stack)` prueft `[r2] & 0x1f`, ruft
`fn_0014015C`, baut die 0x20-Byte-Eintraege und liefert 1/0.
`fn_0013F474` relokalisiert danach die Listen an `ctx+0x58`.

Im Port wird die Kette **nie** erreicht:
- `ARM_WATCH` auf `0x284D24`, `0x11E494`, `0x12493C`, `0x12FC24`,
  `0x139914`, `0x13F2D4` = **0 Treffer**.
- Ursache: `fn_00108F70` (einziger Aufrufer von `0x284D24` bei
  `0x109090`/`0x109514`) wird nicht aufgerufen, weil
  `fn_001034F4` den Aufruf bei `0x103520` nie erreicht.

Ruft man `fn_00108F70(world, world)` per `HLE_FRAME_LOOP2` auf,
laeuft `fn_00284D24` **7x** - aber der GX-Zweig (`0x11E494`) wird
nicht erreicht (`ARM_WATCH=11E494` = 0). Die Submission braucht also
die korrekten Struktur-Argumente (`r2=sp+0x50`, `r3=sp+0x3c`,
Stack `{r0,r1,r2}`) aus `fn_00108F70`.
"""
    s += body
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
