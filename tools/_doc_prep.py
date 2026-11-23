p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Prep-Methode ist 0x001BC450' not in s:
    body = """

### Prep-Methode ist 0x001BC450 - Kette kommt bis fn_0012493C

Der Prep-Aufruf bei `0x285060` (`[world]->vtable+0xA0`) wurde
instrumentiert (`[prep]`):

```
[prep] obj=003EC028 vt=00000000 fn=00000000
[prep] obj=003EC028 vt=003CA494 fn=001BC450
```

Die Methode ist also **`0x001BC450`** (Scene-Vtable `0x003CA494`).
Beim ersten Aufruf ist die Vtable noch 0 (Scene nicht konstruiert).

Mit erzwungenen Gates (`[resforce]` fuer `fn_00123780`,
`fn_0011B98C`, `0x285048`, Prep-Rueckgabe, `0x280034`) erreicht die
Kette:

```
ARM_WATCH=11E494 -> 7
ARM_WATCH=11E4E0 -> 3
ARM_WATCH=12493C -> 3
ARM_WATCH=12FC24 -> 0
```

`fn_0011E494` hat zwei Gates: `[obj+0x710] != 0` und
`fn_00280034 != 0` (beide im Port 0, erzwungen). Danach laeuft
`fn_0012493C`; dessen Gate `fn_00284AA8` (0x124A24) liefert 0,
daher wird `fn_0012FC24bef` (GX-Submit) noch nicht erreicht.

Standardpfad bleibt unberuehrt: TOP 0/96000, BOTTOM 0/76800.
"""
    s += body.replace('fn_0012FC24bef', 'fn_0012FC24')
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
