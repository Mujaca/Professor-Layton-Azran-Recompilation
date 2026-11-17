# Port-Status

Letzte Aktualisierung: 2026-11-18

## Kurzfassung

Der recompilierte Code läuft inzwischen durch **Heap-Setup, `srv`, APT-Initialisierung
(GetLockHandle, Initialize, Enable, SetAppCpuTimeLimit)** bis zu `hid:USER` und `ndm:u`.
Der Menü-Screen ist noch nicht erreicht; der aktuelle Blocker liegt in der
APT-/Fehlerbehandlung (zwei `err:f`-Aufrufe, danach Stillstand).

```powershell
python tools\build_port.py
.\out\port.exe out\romfs
```

## Recompiler-Fixes dieser Runde

| Bug | Wirkung |
|---|---|
| **Bedingte Terminatoren ohne Fallthrough** (`bxeq lr`, `popne {pc}`) | Code sprang nach nicht ausgeführtem Rücksprung in den falschen Block → korrupte Zeiger |
| `lsls/lsrs/asrs` ohne Flag-Update | Heap-Ausrichtungsprüfung der SDK schlug fehl |
| Post-Index-Writeback | BSS-Nullschleife lief endlos |
| `push`/`stmdb`-Writeback (DB/DA) | Stackpointer blieb stehen → SDK las SVC-Argumente falsch |

## HLE-Stand

- **Config-Memory** `0x1FF80000` (Kernel-/Firmware-Version, `app_mem_alloc = 128 MiB`)
- **Kernel-SVCs**: ControlMemory (Bump-Allocator, korrekte SDK-Konvention), ResourceLimits,
  QueryMemory, CreateAddressArbiter, CreateThread (echte **Fibers**), Wait/Sleep (kooperativ),
  GetProcessId, IDs, Handles
- **Mutex-Bootstrap**: null-initialisierte BSS-Semaphore werden beim ersten Warten als frei behandelt
  (`arm_mutex_bootstrap`, Hook im SDK-Lock-Slowpath)
- **Services**: `srv:` (vollständig), `APT:U` (1, 2, 3, 4, 5, 0x4F, 0x28), `hid:USER` (0x0A GetIPCHandles),
  generische Antworten für alle anderen
- **Diagnose**: `HLE_LOG`, `HLE_TRACE=1` (`out/hle_trace.txt`), `ARM_WATCH=0x…`,
  `HLE_SRV_LAYOUT=1|2|3` (Handle-Übergabe-Varianten)

## Boot-Verlauf

```
CreateAddressArbiter → ResourceLimits → ControlMemory (87 MiB @0x08000000,
41 MiB linear @0x14000000) → ConnectToPort("srv:")
srv: RegisterClient, EnableNotification, GetServiceHandle("APT:U")
APT:U: cmd 1 (GetLockHandle), 2 (Initialize), 3 (Enable), 0x4F (SetAppCpuTimeLimit)
err:f: 2× ThrowFatalError  (Ergebniscode 0xF96047A2)
srv: GetServiceHandle("hid:USER") → hid:USER cmd 0x0A (GetIPCHandles)
srv: GetServiceHandle("ndm:u")    → ndm:u cmd 6
→ Stillstand
```

## Nächster Blocker

Zwei `err:f`-Aufrufe (ThrowFatalError) deuten auf einen Fehler in der APT-Phase hin;
zusätzlich werden zwei **virtuelle Methoden über `vtable+0x2C`** aufgerufen, deren
Eintrag 0 ist (Blöcke `0x10799C` und `0x107A4C`; das Ziel wird als „unbekanntes
call-Ziel 0" geloggt). Das Objekt stammt aus der SDK-Initialisierung und wird
offenbar nicht vollständig aufgebaut.

Konkrete nächste Schritte:
1. Ergebniscode `0xF96047A2` dekodieren und die zugehörige APT-Antwort korrigieren.
2. Das Objekt mit `vtable+0x2C == 0` identifizieren (Rückverfolgung von `r7`/Vtable).
3. Danach `fs:USER` (RomFS auf `out/romfs`) und `gsp::Gpu` (Command-Listen/Swap).
4. PICA-2D-Renderer (TEV-Subset) → Menü-Screen im Fenster.

## Werkzeuge

```powershell
$env:PYTHONPATH = "tools"
$env:HLE_LOG = "1"; $env:HLE_TRACE = "1"; $env:HLE_IPC_LOG = "400"
$env:ARM_WATCH = "12E1F0"
python tools\build_port.py
.\out\port.exe out\romfs --shot out\shot.bmp
```
