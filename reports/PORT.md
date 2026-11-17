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
| 1 | `arm_pool_alloc` gab **Host-Zeiger** (64->32 Bit abgeschnitten) zurueck | SDK-Objekte lagen ausserhalb des Gast-Adressraums |
| 2 | `SERVICE_NAMES[33]` out-of-bounds (SVC_FILE) | `printf` las Muell-Zeiger, Boot hing bei Datei-IPCs |
| 3 | fs-Reply-Layout war 3+2 statt **1+2** (Result + MoveHandle) | SDK wertete jede Antwort als Fehler -> `err:f` |
| 4 | TLS-Felder `+0x8C` als "Reply-Groesse" ueberschrieben | Reply-Wort 3 (Signal/Param) zerstoert |
| 5 | `APT:U 0x0D/0x0E` (ReceiveParameter) fehlte | Endlos-Warteschleife auf `Wakeup` |
| 6 | `scan.py`: **`blx reg` als Blockende** | Code nach indirekten Aufrufen fehlte |
| 7 | Thumb-Init-Loop `0x100974` rief nur den 1. Konstruktor | `add r4,#4`/`bne` fehlten (Folge von #6) |
| 8 | **SP-Korruption** durch defekte Epiloge | Stackframes der Konstruktoren zerstoert |
| 9 | Konstruktorziele waren keine Seeds | `arm_call_guest` -> No-Op |
| 10 | **`vldr`/`vstr`/`ldrd`/`strd` mit `[pc, #imm]`** nutzten `c->r[15]` (nie gepflegt) statt `pc+8` | Float-Konvertierung las Muell -> Endlosschleife `0x24B2DC` |
| 11 | Thumb-`main`-Entry `0x0010009D` fehlte als Seed | `_start` rief ins Leere |
| 12 | **Keine Gast-Threads**: `svcCreateThread` legte nur ein Handle an | SDK-Worker liefen nie, Warteschleifen blockierten |
| 13 | `svcGetSystemTick` lieferte nur 32 Bit | Timeout-Vergleiche (64 Bit) liefen nie ab |
| 14 | `svcArbitrateAddress` ohne Decrement | Futex-/Wartecounter bewegte sich nicht |

## Threading-Modell (neu)

- Jeder Gast-Thread = Host-Thread mit eigenem `ArmCpu` (gemeinsamer Speicher,
  eigenes TLS-Fenster `0x1FF82000 + n*0x1000`).
- Ein globaler `CRITICAL_SECTION` sorgt dafuer, dass immer nur **ein** Gast-
  Thread laeuft (wie eine Single-Core-Konsole).
- Blockierende SVCs (`SleepThread`, `WaitSynchronization*`, `ArbitrateAddress`)
  geben den Lock kurz ab (`guest_yield`), damit andere Threads laufen.

## Aktueller Stand

Der Konstruktor-Init laeuft durch; das Spiel startet seine Worker-Threads
(bis zu 11 Gast-Threads mit eigenem TLS-Fenster) und initialisiert
`fs`, `cfg`, `ptm`, `hid`, `ndm`, `dsp`, `y2r`, `APT`. Danach laeuft es in
einer Frame-/Notification-Schleife und oeffnet Dateien (`fs:USER 0x0802`,
u. a. `/gamecoin.dat`).

**`gsp::Gpu` wird weiterhin nicht geoeffnet** -> kein Frame. Der Haupt-Thread
haengt in einer SDK-Mutex-/Futex-Schleife (`0x287B90`, `0x288B80`).

### Beobachtete IPC-Verteilung (20 s Lauf)

```
450 x srv:  0x000B   (ReceiveNotification)
226 x srv:  0x0050
 75 x ?     0x0001   (Handle-Query)
 26 x srv:  0x0000
  9 x srv:  0x0005
  3 x cfg:u 0x0001
  3 x ptm:u 0x000C / 0x000B
```

### FS-Parameter (aus `[fsdbg]`)

- `0x0803 OpenFileDirectly`: `n=8 t=4`, Binary-Pfad (12 Byte) im ersten
  Translate-Puffer -> funktioniert.
- `0x0802 OpenFile`: zwei Varianten beobachtet (`n=7` mit Wchar16-Pfad
  `/gamecoin.dat`, und `n=3` ohne Pfad). Die `n=3`-Variante liefert noch
  keinen Pfad -> leere Dateien.

### Fixes dieser Runde

| Fix | Wirkung |
|---|---|
| `vldr`/`vstr`/`ldrd`/`strd` mit `[pc,#imm]`: `pc+8` statt `c->r[15]` | Float-Konvertierungs-Endlosschleife `0x24B2DC` behoben |
| Seed `0x0010009D` (Thumb-`main`) | `_start` rief ins Leere |
| Kooperative Gast-Threads | Worker-Threads laufen wirklich |
| `svcGetSystemTick` 64 Bit | Timeout-Vergleiche laufen ab |
| `svcArbitrateAddress` dekrementiert | Futex-Zaehler bewegt sich |
| `dsp::DSP 0x0011` mit `(Result, true, MappedBuffer)` | DSP-Retry-Loop beendet |
| `APT:U 0x0050` mit 2 Normal-Woertern | APT-Loop entschaerft |
| Bus-Alias `0x8xxxxxxx`/`0xBxxxxxxx` maskiert | Gast-Fehlzugriffe beseitigt |
| FS-Pfad aus Translate-Deskriptor `{flags,addr}` | Wchar16-Pfade werden gelesen |

### Naechster Schritt

1. SDK-Mutex-Warteschleife (`0x287B90` / `0x288B80`) korrekt bedienen, damit
   der Haupt-Thread die Grafik-Initialisierung erreicht.
2. `gsp::Gpu` implementieren (WriteHWRegs, SetCommandList,
   RegisterInterruptRelayQueue, SetBufferSwap) + PICA-2D-Renderer auf
   `0x1F000000`.
3. Menue-Frame per `out\port.exe out\romfs --shot out\shot.bmp` belegen.

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
