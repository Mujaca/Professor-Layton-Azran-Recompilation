# Port-Status

Letzte Aktualisierung: 2026-11-18 (Blocker-Analyse)

## Abstand zum Menü (Schätzung)

| Bereich | Stand |
|---|---|
| Kernel/Heap/Threads | ~90 % (Heap, SVCs, Fibers, Handles) |
| `srv:` / `APT:U` | ~80 % (Init-Kette läuft) |
| `err:f`, `hid`, `ndm` | ~30 % (Stubs) |
| `fs:USER` (Assets) | 0 % |
| `gsp::Gpu` + PICA-Renderer | 0 % — **größter Posten** |
| **Gesamt** | **~2–4 Arbeitstage** bis zum ersten Menü-Frame |

## Blocker-Analyse

**Blocker 1 (Fatalcode `0xF96047A2`)**
Der Code ist **kein dekodierter Fehler**, sondern ein hartkodiertes Literal im Fehlerpfad von
`0x1078F4` (Literal bei `0x107A70`). Die Funktion ist ein IPC-Wrapper *mit Buffern*
(`0x287E38`); nach dem Aufruf prüft sie `[sp+8] == 0x28` und eine berechnete Größe `r6 > 0`.
Beides erwartet eine **vom Kernel geschriebene Reply-Größe**, die unsere HLE nicht liefert
→ Fehlerpfad → `err:f::ThrowFatalError`.

**Blocker 2 (NULL-Vtable `vtable+0x2C`)**
Kein eigener Bug: Der Aufruf passiert **im selben Fehlerpfad** (`0x107A4C`), nachdem `r7` auf 0
gesetzt wurde (`moveq r7,r0` bei fehlgeschlagenem Objekt-Create). Die Vtable selbst ist intakt
(`0x3CB3E8`, `+0x2C = 0x33E054`). Fix von Blocker 1 beseitigt Blocker 2 automatisch.

## Nächster konkreter Schritt

Reply-Größen-Semantik der SDK-IPC implementieren:
1. Beim `svcSendSyncRequest` die Response-Wortzahl in den vom Wrapper erwarteten Out-Slot
   schreiben (Wrapper `0x287E38`, Aufrufer `0x1078F4`).
2. Danach läuft die APT/err-Kette ohne Fatalpfad weiter; als Nächstes `fs:USER`.

## Aktueller Crash

Nach APT/hid/ndm läuft der Boot weiter und stürzt bei `0x134AAC` ab (kleiner Copy-Helper
`*r1 = *r0` mit `r1 = 0xFFD76260`, also einem ungültigen Zeiger — vermutlich Folge der
Reply-Größen-Thematik).

## Recompiler-Fixes dieser Session

| Bug | Wirkung |
|---|---|
| Bedingte Terminatoren ohne Fallthrough (`bxeq lr`, `popne {pc}`) | Sprung in falschen Block → korrupte Zeiger |
| `lsls/lsrs/asrs` ohne Flags | Heap-Ausrichtung schlug fehl |
| Post-Index-Writeback | BSS-Nullschleife endlos |
| `push`/`stmdb`-Writeback (DB/DA) | Stackpointer blieb stehen |

## HLE-Stand

Config-Memory, ControlMemory (Bump-Allocator), ResourceLimits, QueryMemory,
CreateAddressArbiter, Threads (Windows-Fibers), Wait/Sleep, Mutex-Bootstrap,
`srv:` komplett, `APT:U` (1,2,3,4,5,0x28,0x4F), `hid:USER` (0x0A), generische Antworten.

```powershell
python tools\build_port.py
.\out\port.exe out\romfs
```
