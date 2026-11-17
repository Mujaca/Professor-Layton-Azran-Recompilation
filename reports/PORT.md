# Erste lauffähige Version (Port-Skelett)

Stand: 2026-11-17

## Was läuft

`out\port.exe` startet, öffnet ein natives Win32-Fenster, lädt den **recompilierten
Spielcode** (17.087 ARM- + 64 Thumb-Funktionen, 1,18 Mio. Instruktionen) und führt ihn
mit einer HLE-Schicht aus. Angezeigt werden:

- das Spiele-Icon/Titel aus der **SMDH** (`icon.bin`) als Startscreen,
- eine Statuszeile mit PC/Halt-Zustand,
- der rohe Framebuffer-Bereich (`0x1FF00000`, 400×240) — sobald das Spiel zeichnet.

```powershell
python tools\build_port.py
.\out\port.exe out\romfs                # Fenster
.\out\port.exe out\romfs --shot out\shot.bmp   # Screenshot + Ende
```

## Was in dieser Runde dazukam

| Baustein | Datei | Inhalt |
|---|---|---|
| HLE-Schicht | `runtime/hle/hle.c` | IPC-Parser (Header-Layout aus Azahar verifiziert), Handle-Tabelle, `srv:` (RegisterClient/EnableNotification/RegisterService/GetServiceHandle), SVCs (ControlMemory, Threads, Sync, GetSystemInfo/ProcessInfo, ResourceLimits, ConnectToPort, SendSyncRequest …) |
| Boot-Host | `tests/boot.c` | 768 MB Speichermodell (FCRAM + VRAM bis 0x20500000), Code-Load, TLS, Watchdog, Ereignis-Ringpuffer |
| Port-Host | `tests/port.c` | Win32-Fenster, Spiel-Thread, Framebuffer-Blitten, SMDH-Startscreen, Screenshot |
| Fenster | `runtime/platform/window.c` | GDI-Fenster, 32-Bit-DIB, Skalierung 2×, Statuszeilen |
| Dispatcher | `generiert` | `arm_call_guest()` mit sortierter Adresstabelle (ARM/Thumb-Bit) — indirekte Aufrufe (`blx rN`) funktionieren jetzt |
| Coverage | `tools/recomp/scan.py` | `--seed-pointers`: alle Wörter im Image, die auf Code zeigen, als Funktionskandidaten → **81,85 %** Textabdeckung (vorher 71,5 %) |
| Recompiler-Fix | `tools/recomp/emit.py` | Post-Index-Schreibweise (`ldr/str rX, [rY], #imm`) setzt jetzt Writeback — vorher Endlosschleife beim BSS-Nullsetzen |
| Build/Auto-Iteration | `tools/build_port.py`, `tools/port_build.py` | Build-Skript; `port_build.py` sammelt fehlende Call-Ziele und iteriert automatisch |

## Aktueller Stand des Boots

Der Code läuft durch die SDK-Initialisierung bis zur Heap-/Ressourcen-Abfrage und
danach in `srv:` (EnableNotification). Danach folgt ein `svcBreak` (Panic) und ein
`memcpy` mit ungültigen Zeigern (Absturz bei `0x30000000`).

Ereignisfolge (aus dem Ringpuffer):

```
svc 0x38 GetResourceLimit
svc 0x3A GetResourceLimitCurrentValues
svc 0x23 CloseHandle
svc 0x3C Break            <- Panic: Heap-/Ressourcen-Setup schlaegt fehl
svc 0x2D ConnectToPort("srv:")
svc 0x32 SendSyncRequest  (srv: EnableNotification)
```

Der Break kommt also **vor** dem ersten Service-Aufruf: die HLE liefert bei der
Heap-Berechnung (Resource-Limit-Werte / `svcControlMemory`) noch keine plausiblen
Werte. Das ist der nächste konkrete Ansatzpunkt.

## Nächste Schritte bis zum Menü

1. **Heap korrekt modellieren**: `svcControlMemory` (Bump-Allocator in FCRAM),
   `svcGetResourceLimit*` mit realistischen Werten, `svcQueryMemory`.
2. **Threads** über Windows-Fibers (`svcCreateThread`/`ExitThread`/`WaitSynchronization`)
   — GSP/HID laufen auf eigenen Threads.
3. **fs:USER**: `romfs:`-Pfade auf `out/romfs/`, Datei-Handles, `ReadFile` mit echten Daten.
4. **apt:U**, **hid:USER**, **gsp::Gpu** (Register-Schreiben, Interrupt-Queue, Swap).
5. **PICA-Renderer** für den 2D-Anteil (TEV-Subset, Texturen ETC1/RGBA8) → erst
   damit kann der Menü-Screen tatsächlich gezeichnet werden.

## Reproduktion

```powershell
$env:PYTHONPATH = "tools"
python -m recomp.cli scan --in out\exefs\code_decomp.bin --prologues --seed-pointers
python tools\build_port.py
.\out\port.exe out\romfs --shot out\shot.bmp
```
