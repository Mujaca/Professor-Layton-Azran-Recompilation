# Port-Status

Letzte Aktualisierung: 2026-11-18 (Static-Init-Durchbruch)

## Kurzfassung

Der Boot kommt jetzt **deutlich weiter als der alte `err:f`-Fatalpfad**: Die
TLS-Reply-Semantik stimmt, `fs:USER`/`cfg:u`/`APT:U` antworten korrekt, und die
**C++-Konstruktortabelle (304 Einträge, 0x3D0648..0x3D0B08) wird ausgeführt**.
Aktuell hängt das Programm in einem der Konstruktoren (Endlosschleife); das
Menü rendert daher noch nicht.

## Diese Session behobene Fehler

| # | Fehler | Wirkung |
|---|---|---|
| 1 | `arm_pool_alloc` gab **Host-Zeiger** (64→32 Bit abgeschnitten) zurück | SDK-Objekte lagen außerhalb des Gast-Adressraums (`0x8843D090`) → Absturz |
| 2 | `SERVICE_NAMES[33]` (SVC_FILE) **out of bounds** | `printf` las Müll-Zeiger → Boot blieb bei jedem Datei-IPC hängen |
| 3 | IPC-Reply-Layout für `OpenFile`/`OpenFileDirectly`/`OpenArchive` war 3+2 statt **1+2** (Result + MoveHandle) | SDK wertete jede Antwort als Fehler → `err:f`-Fatalpfad |
| 4 | TLS-Felder `+0x8C` wurden als "Reply-Größe" überschrieben | **Reply-Wort 3 (= Signal/Param) wurde zerstört**; APT meldete nie `Wakeup` |
| 5 | `APT:U 0x0D/0x0E` (ReceiveParameter/GlanceParameter) fehlte | Spiel wartete endlos auf `SignalType::Wakeup` |
| 6 | `scan.py`: **`blx reg` wurde als Blockende** behandelt | Code nach jedem indirekten Aufruf ging verloren |
| 7 | Thumb-Init-Loop `0x100974` rief nur den **ersten** Konstruktor | `r4 += 4` / `bne` fehlten im Block (Folge von #6) |
| 8 | **SP-Korruption** durch fehlerhafte Epiloge (z. B. `fn_00291BDC`) | Konstruktoren sahen zerstörte Stackframes |
| 9 | Konstruktor-Ziele waren **keine Funktions-Seeds** | `arm_call_guest` → No-Op, Init lief ins Leere |

## Wichtige Erkenntnis zum Programmaufbau

- Entry `0x00100000` → `_start`; `blx 0x100974` ist **Thumb** und läuft die
  Konstruktortabelle `0x3D0648..0x3D0B08` ab (304 rel. Pointer).
- Die früher beobachteten `fs:USER`/`hid`/`cfg`/`gsp`-Aufrufe kamen aus dem
  **Exit-Pfad** (Destruktoren), nicht aus `main`.
- `main` liegt in der Konstruktorkette; erst nach vollständigem Init ist mit
  GSP/PICA zu rechnen.
- `__libc_init_array` bei `0x100BB0` ist separat und wirklich leer
  (`0x3D0B08..0x3D0B08`).

## Aktueller Blocker

Endlosschleife in der Konstruktorausfuehrung: Block **`0x0024B2DC`**
(Float-nach-Int-Konvertierung) wird von `0x00382C40` aufgerufen und terminiert
nicht.

Der Code ist:

```
0x24B2C8  ldr  r2,[pc,#0x74]     ; 0x47800000 = 65536.0f
0x24B2D4  vldrge s2,[pc,#0x6c]   ; 0x47800000 = 65536.0f
0x24B2DC  vsub.f32 s1,s1,s2
0x24B2E0  vmov r3,s1
0x24B2E4  cmp  r3,r2
0x24B2E8  bge  0x24B2DC
```

Das ist eine Bereichsreduktion fuer grosse Floats; sie terminiert nur bei
endlichem `s1`. Upstream entsteht offenbar **+Inf** (frueher beobachteter
Fehlzugriff auf `0x7F800000`), z. B. durch `vdiv.f32 s0,s1,s2` bei `0x00382C44`
mit `s2 = 0`.

Naechster Schritt: die VFP-Kette ab `0x382C40` pruefen (`vldmia sp`, `vdiv`,
s1/s2-Werte) und die Ursache des Inf bzw. 0-Divisors beseitigen. Danach sollten
die restlichen Konstruktoren und der eigentliche Spiel-Init-Code laufen.

## Umgesetzte HLE-Bausteine

- Config-Memory, ControlMemory (Bump), QueryMemory, GetSystemInfo, Threads,
  Mutex-Bootstrap, `srv:` komplett, `APT:U` (1,2,3,4,5,0x0D,0x0E,0x4F),
  `hid:USER` (0x0A), `ndm:u`, `ptm:u` (Stub), `cfg:u` (GetConfig/Region/Model),
  `fs:USER` (OpenFile, OpenFileDirectly, OpenArchive/Control/Close, CreateFile,
  GetFreeBytes) + Datei-Service (Read/Write/GetSize/SetSize/Close) auf `out/romfs`.
- Reply-Semantik: Puffer liegt in `TLS+0x80`; Wort 1 = Result, danach Normal-
  Parameter, danach Translate-Parameter (MoveHandle = `0x10`, Handle).

## Kommandos

```powershell
python tools\build_port.py
python tests\mvp_selftest.py        # 13/13
.\out\port.exe out\romfs --shot out\shot.bmp
```
