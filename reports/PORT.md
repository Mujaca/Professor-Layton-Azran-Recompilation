# Port-Status

Letzte Aktualisierung: 2026-11-17 (abends)

## Kurzfassung

Der recompilierte Code bootet jetzt durch die **SDK-Initialisierung** (Heap, Ressourcen,
`srv`-Verbindung, APT-Handle) bis in die Applet-Initialisierung. Der Menü-Screen ist noch
nicht erreicht; aktueller Blocker ist ein **Mutex-Spinlock in der APT-Phase**.

```powershell
python tools\build_port.py
.\out\port.exe out\romfs
```

## Was in dieser Runde gefixt wurde (Recompiler)

| Bug | Wirkung |
|---|---|
| **Fehlender Fallthrough** zwischen Blöcken | Blöcke wurden in Fundreihenfolge ausgegeben; ohne explizites `goto` sprang der Code in den falschen Block (Ursache für viele „seltsame" Sprünge) |
| **`lsls/lsrs/asrs` ohne Flag-Update** | SDK prüfte Ausrichtung über `lsls …; bne` → Heap-Allokation schlug fehl |
| **Post-Index-Writeback** (`ldr/str rX, [rY], #imm`) | BSS-Nullschleife lief endlos |
| **`push`/`stmdb`-Writeback** (DB/DA) | Stackpointer wurde nicht dekrementiert → SDK las ihre Argumente vom falschen Stack-Slot |

Alle 13 Semantik-Tests laufen weiterhin (`python tests\mvp_selftest.py`).

## Was in dieser Runde dazukam (HLE)

- **Config-Memory** (`0x1FF80000`) initialisiert: Kernel-/Firmware-Version, `ns_tid`,
  `app_mem_alloc = 128 MiB`, `sys_mem_alloc`, `base_mem_alloc`
- **`svcControlMemory`** korrekt: Op/Flags aus `r0`, Permissions aus `r4`, Out-Pointer vom
  Stack; Ergebnis in `r1` (SDK-Wrapper-Konvention); Bump-Allocator für App-/Linear-Heap
- **`svcGetResourceLimit*`**, `QueryMemory`, `CreateAddressArbiter` mit Werten/Handles
- **`srv:`** vollständig genug: RegisterClient, EnableNotification, RegisterService,
  GetServiceHandle — inkl. Handle-Übergabe in **beiden** Layouts (normale Parameter und
  Translate-Deskriptor), weil die Nintendo-SDK das Handle als normalen Parameter liest
- **`APT:U`**: GetLockHandle, Initialize, Enable, Finalize, NotifyToWait, GetAppletManInfo
- **Watchpoint-Diagnose**: `ARM_WATCH=0x…` loggt Register + Speicher an beliebigen Blöcken
- **SVC-/IPC-Trace**: `HLE_TRACE=1` schreibt `out/hle_trace.txt`

## Aktueller Boot-Verlauf (Log)

```
svc 0x21 CreateAddressArbiter
svc 0x38 GetResourceLimit / 0x3A CurrentValues / 0x23 CloseHandle
svc 0x01 ControlMemory (ALLOC 87 MiB @0x08000000, ALLOC_LINEAR 41 MiB @0x14000000)
svc 0x2D ConnectToPort("srv:")
ipc srv cmd 0x1 (RegisterClient)
ipc srv cmd 0x2 (EnableNotification)
ipc srv cmd 0x5 ("APT:U") -> Handle
ipc APT:U cmd 0x1 (GetLockHandle)
-> danach Spinlock (nn::os-Mutex) bei 0x12E1F0
```

## Nächster Blocker

Der Mutex bei `0x12E1F0` wartet auf einen **positiven** Wert (Konvention: >0 = frei,
<0 = belegt). Die Objekte liegen in `.bss` (0x3EB000–0x456168) und sind nach dem
BSS-Nullen 0 — die C++-Statik-Initialisierer (`__libc_init_array`) setzen sie offenbar
nicht (oder werden von uns noch nicht korrekt ausgeführt).

Nächste Schritte:
1. `__libc_init_array` verifizieren (Init-Array-Tabelle ablaufen, Konstruktoren aufrufen)
   — damit werden alle globalen Mutexe/Objekte korrekt initialisiert.
2. Danach APT-Initialisierung fortsetzen (`Initialize`, `Enable`, `GetSharedFont`).
3. `fs:USER` (RomFS auf `out/romfs`, Datei-Handles, `ReadFile`).
4. `hid:USER`, `gsp::Gpu` (Register/Command-Listen, Swap) und der PICA-2D-Renderer.

## Werkzeuge für die Weiterarbeit

```powershell
$env:PYTHONPATH = "tools"
$env:HLE_LOG = "1"           # IPC/SVC-Log
$env:HLE_TRACE = "1"         # out/hle_trace.txt
$env:ARM_WATCH = "12E1F0"    # Register+Speicher an diesem Block
$env:HLE_MUTEX_HACK = "1"    # Workaround: erster Mutex auf 1 setzen
python tools\build_port.py
```
