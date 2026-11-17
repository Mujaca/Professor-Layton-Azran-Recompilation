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

Der Boot laeuft (Konstruktor-Init, bis zu 11 Gast-Threads, ~233.000 IPCs in
8 s), initialisiert `fs/cfg/ptm/hid/ndm/dsp/y2r/APT` – aber **`gsp::Gpu` wird
weiterhin nicht geoeffnet** (0 Treffer) -> kein Frame.

### Wichtige Korrekturen an den IPC-Reply-Deskriptoren

Nach Azahar `apt.cpp` / `srv.cpp` / `hid`:

| Dienst | vorher | jetzt |
|---|---|---|
| `APT:U 0x0002 Initialize` | 3 Translate-Woerter deklariert, aber **5 geschrieben** (`0x00` + 2x `{0x10,handle}`) -> SDK las `0x10` als zweiten Handle | `{0x04000000 (CopyHandle x2), ev1, ev2}` = exakt 3 Woerter |
| `APT:U 0x0001 GetLockHandle` | MoveHandle `0x10` | CopyHandle `0x00` |
| `hid:USER 0x000A GetIPCHandles` | Deskriptor `0x00` (1 Handle) bei 6 Handles | `0x14000000` (CopyHandle x6) |
| `srv: 0x0005 GetServiceHandle` | Standard 2 Normal + 2 Translate | Standard 1 Normal + 2 Translate (Citra `MakeBuilder(1,2)`) |

Effekt: `y2r:u` wird jetzt als eigener Dienst geoeffnet; der Haupt-Thread
kommt weiter, erreicht aber die Grafik-Initialisierung noch nicht.

### Weitere Fixes

- `APT:U 0x000D/0x000E` "kein Parameter" liefert jetzt den korrekten
  Result-Code `0xC8A0CFEF` (NoData=1007, Applet=51, InvalidState=5, Status=25)
  statt eines geratenen Werts.
- `svcGetThreadId` (0x37) liefert pro Thread eine **eindeutige** ID
  (`0x100 + TLS-Index`) statt konstant `0x100` (wichtig fuer Mutex-Besitz).

### Naechster Schritt

Weiterhin offen: der Haupt-Thread bleibt in APT-/Semaphor-Poll-Schleifen
(`0x141EA4`, `0x288B80`, `0x11A818`). Als Naechstes `svcArbitrateAddress`
typkorrekt blockieren (echte Warteschlange) und `srv:0x000B`-Poll durch echtes
Event-Blocking ersetzen; danach `gsp::Gpu` + PICA-2D-Renderer auf `0x1F000000`.

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
