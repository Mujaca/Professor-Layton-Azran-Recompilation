p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'fn_0012493C-Gates' not in s:
    body = """

### fn_0012493C-Gates: 0x26F130 und 0x130220 erzwungen

`fn_0012493C` hat mehrere `beq 0x124C2C`-Gates. Instrumentierung
(`[gx]`) im Komplettlauf:

```
[gx] 0x26F130 -> 0 (erzwungen 1)
[gx] 0x130220 -> 1 (1)
ARM_WATCH=12FC24 -> 0
```

Damit sind zwei Gates bekannt und erzwungen:
- `0x26F130` (0x124978): liefert 0,
- `0x130220` (0x124988): liefert 0.

Danach folgen weitere Gates:
`0x13C46C`/`0x284AA8` (0x124AAC), `0x1300C8`, `0x13C4EC`/
`0x284AA8` (0x124AEC), `0x130170`, danach `b 0x399F80`
(RAM-Stub -> `fn_0012FC24`).

`fn_0012FC24`/`0x13F2D4` bleiben bei 0 Treffern; `qops=0` und
192 HLE-`SetCommandList` (`lr=001ED9C8`).

Prep-Methode bleibt `[world]->vtable+0xA0 = 0x001BC450`
(Vtable `0x003CA494`).
"""
    s += body
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
