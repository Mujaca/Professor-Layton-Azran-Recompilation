p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'fn_0012493C-Gates erzwungen' not in s:
    body = """

### fn_0012493C-Gates erzwungen - fn_0012FC24 laeuft

Die vier Gates in `fn_0012493C` (`0x284AA8` bei `0x1249AC` und
`0x1249E4`, `0x1300C8` bei `0x1249C0`, `0x130170` bei `0x1249F8`)
wurden auf 1 erzwungen (`tools/_patch_gen_2493c.py`).

Ergebnis im Komplettlauf (`out/logs/c2493_log.txt`):

```
ARM_WATCH=12FC24 -> 8
ARM_WATCH=12FD38 -> 0
ARM_WATCH=139914 -> 0
ARM_WATCH=13F2D4 -> 0
Trigger ... qops=0
SetCommandList: 192, alle lr=001ED9C8
```

`fn_0012FC24` ist der **Kommando-Emitter** (schreibt PICA-Woerter
ueber `fn_00275994`/`fn_0026926C` und liefert 1). Der eigentliche
Submit-Einstieg ist die separate Funktion **`0x12FD38`**
(`bl 0x139914` bei `0x12FDA8`), die keinen statischen Aufrufer hat
und im Port nicht laeuft.

Damit ist der Emitter erreicht, aber der Submit
(`0x12FD38 -> 0x139914 -> 0x13F2D4`) noch nicht.
"""
    s += body
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
