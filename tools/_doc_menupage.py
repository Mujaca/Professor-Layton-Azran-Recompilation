p = 'docs/PORT_STATE.md'
s = open(p, encoding='utf-8').read()
if 'Menue-Seiten-Setup wird per Menue-Objektzaehler' not in s:
    body = """

### Menue-Seiten-Setup wird per Menue-Objektzaehler uebersprungen

Korrektur zum vorherigen Befund: Die `.fa`-Registrierung wird nicht
dauerhaft uebersprungen. Mit 900 Frames (`out/logs/late_log.txt`) hat
`ARM_WATCH=2919DCutuell` **3 Treffer** - `lt6_a.fa`, `lt6_b.fa`,
`lt6_uk.fa` werden also registriert. Der Init braucht nur lange.

Der eigentliche Blocker fuer den Menueaufbau ist der Zaehler
`menu+0x40` (`0x3EE16C`): `fn_00100F98` prueft bei `0x101508`
`fn_0010F188` = `u16[world+0x2144]` (= `menu+0x40`). Ist er != 0,
wird das Menue-Seiten-Setup `fn_0028F7BC` bei `0x10153C`
uebersprungen. Die HLE-Initialisierung setzt genau dieses Feld auf 1
(`runtime/hle/hle.c`; `HLE_MENU_OBJ0` schaltet es auf 0).

Mit `HLE_MENU_OBJ0=1` (und dem Skip des blockierenden zweiten
`fn_001034F4`, `r0=1`) laufen die echten Setup-Funktionen:

```
[flowdbg] 1014EC reached r4=00000001
ARM_WATCH=28F7BC -> 572+ Treffer
  r0=003EC028 r1=000003E8 r2=FFFFFFFF r3=FFFFFFFF lr=00101540
ARM_WATCH=2800C8 -> 600+ Treffer (Engine-Draw-Builder)
  r0=003EE12C (Menue-Objekt) r1=0CF09944 r2=00000002
```

`fn_0028F7BC` ruft also den echten Draw-Builder `fn_002800C8` mit dem
Menue-Objekt `0x3EE12C` auf. Der Builder bricht aber bei `0x280100`
ab, weil `menu+0x6A` (`0x3EE196`) = 0 ist; `HLE_DRAW_LIST=1` legt
zwar Array/Cap an (Status `dl=0/256`), aber der Count bleibt 0.

**Zusatzbefund Standardlauf:** Mit dem aktuellen Build laeuft der
Engine-2D-Renderer `fn_001ED8FC` im Normallauf jetzt mit
(`ARM_WATCH=1ED8FC` -> **280 Treffer** in 150 Frames). Der
HLE_LOG-Lauf zeigt **2703** `SetCommandList` (weiterhin
`lr=001ED9C8`, HLE-Eintrag) plus eine Engine-Liste
`addr=0x0CFE6000` (`out/logs/std3_log.txt`). Bei 150/210 Frames ist
das Bild exakt 0/96000 + 0/76800; bei 180 Frames landet der
Screenshot jetzt mitten im Frame (Tearing, weil laufend gerendert
wird).
"""
    s += body.replace('2919DCutuell', '2919DC')
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('ok')
else:
    print('already')
