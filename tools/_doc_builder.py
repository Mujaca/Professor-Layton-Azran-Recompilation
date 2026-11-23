p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Draw-Builder haengt jetzt Eintraege an' not in s:
    body = """

### Draw-Builder haengt jetzt Eintraege an (menu+0x6A geklaert)

`menu+0x6A` (`0x3EE196`) ist der Freislot-Zaehler des Pools bei
`menu+0x5C`. Der Builder `fn_002800C8` liest ihn bei `0x2800F0` und
dekrementiert ihn bei `0x28015C`; der Slot selbst kommt aus
`fn_00284A38` = `[container] + [container+4]*index`. Der Konstruktor
nullt den Zaehler, die HLE-Initialisierung setzte bisher nur den
Index (`0x3EE194`) auf 1.

Mit `arm_st(0x3EE196, 0x100)` + `arm_st(0x3EE194, 0)` vor der
Init-Schleife (`build/gen_boot.c`, `0x1014EC`) und
`HLE_DRAW_LIST=1`, `HLE_MENU_OBJ0=1`, `HLE_ENGINE_MENU=1`:

```
[mw] 003EE184 <- 00000001 size=2 pc=002801A4 lr=00280158
[mw] 003EE184 <- 00000000 size=2 pc=0028DFA8 lr=0028DE78
[port] dl=0/256 arr=0CFF0000 e0=0D001E00 mn=128
```

Der Builder schreibt also einen Objektzeiger in die Objektliste
`menu+0x50` (Count bei `0x3EE184`), und der Menue-Drain konsumiert
ihn wieder. Der Zeiger `0x0D001E00bac` stammt aber aus dem
HLE-Scratch-Pool; das Objekt hat keine echte Klassen-Vtable.

Test mit dem echten Menue-Objekt in allen Pool-Slots
(`HLE_POOL_MENUOBJ=1`): weiterhin 192 `SetCommandList`, alle
`lr=001ED9C8` und `addr=0x0CFE6000` (HLE-Liste). Der 2D-Ring
`0x40ABC0` enthaelt also unveraendert nur den HLE-Eintrag.
"""
    s += body.replace('0D001E00bac', '0D001E00')
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
