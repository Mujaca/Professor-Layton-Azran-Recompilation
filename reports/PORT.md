# Port-Status

Letzte Aktualisierung: 2026-11-19

## Ziel

Erste lauffaehige Version des Hauptmenues: das **offiziell vom Spiel gerenderte**
Hauptmenue im Port (nicht der Port-Platzhalter `draw_menu`).

## Kurzfassung

Der Port fuehrt das Spiel aus (Static Recompilation, HLE, Win32-GDI-Anzeige).
Alle **HLE- und Grafik-Teile sind belegt funktionsfaehig**, aber das Spiel
erzeugt im Port **keine PICA-Kommandoliste** und rendert daher nichts.

* Platzhalter-Menue: `draw_menu` (echtes SMDH-Icon + Spieltitel, `--keys`)
* Screenshot: `out/shot.bmp` (288.054 Byte, 400x240)
* Selbsttests: 13/13

## Verifizierter Stand

### HLE / Services

| Punkt | Beleg |
|---|---|
| 13 Dienste, natuerlicher `srv:GetServiceHandle "gsp::Gpu"` | `[srvname] "gsp::Gpu" ... -> 50` |
| GSP-Init: `0x0016`, `0x0013`, `0x0010`, `0x001E` | `HLE_LOG=1` |
| FS mit echten Pfaden (`/gamecoin.dat`) | `[fs] OpenFile "/gamecoin.dat" -> ok` |
| 0 Fatals | `cmd=0x0001 n=32` = 0 |
| APT-Wakeup, DSP-Audio, VBlank-Thread | Logs |
| Reply-Formen gegen Azahar korrigiert (APT Initialize, Y2R 0x000D/0x000F, DSP 0x0002) | `tools/azahar/src/core/hle/service/...` |

### Grafik-Pipeline (technisch vollstaendig)

| Punkt | Beleg |
|---|---|
| Mini-PICA-Ausfuehrer + Rasterizer (`runtime/gpu.c`) | `[gputest] ops=1 fb=1F000000 px=00FF00` |
| `gsp SetCommandList (0x0006)` merkt Adresse/Groesse | `[gsp] SetCommandList addr=0x00410000 size=0x1C` |
| `TriggerCmdReqQueue (0x000C)` fuehrt die Liste aus | `[gsp] TriggerCmdReqQueue -> 1 Ops` |
| VRAM-Writes (memwatch, Muster `mw] 1F`) | 400 Treffer |
| Bildschirm: Port zeigt VRAM-Inhalt | `out/shot.bmp` = 400x240, alles `(0,255,0)` (Testmuster) |
| `SetBufferSwap (0x0005)` schaltet Framebuffer (`gpu_set_fb`) | Code |
| Spielcode-Sender `fn_0013E3E0` (`r0` = Zeiger auf Handle-Slot) sendet `0x000C` | `[ipc] blk=0013E3E0 gsp::Gpu cmd=0x000C h=40` |

### Spielseite (offener Punkt)

| Punktbefund | Beleg |
|---|---|
| Spiel baut/ sendet **keine** PICA-Liste | GSP-Shmem `0x10002000..` nur 1 Nullwort |
| Trigger `fn_0013E3E0` wird nie ausgefuehrt | `ARM_WATCH=13E3E0` -> 0 |
| Alle 7 `0x000C`-Sendestellen identifiziert | Header-Literale `000C0040/80/82/0104` |
| `gsp::Gpu` im Normallauf | nur `0x0010/0x0013/0x0016/0x001E` |
| `arc/lt6_a.fa` wird nie geoeffnet | 0 FS-Open, 0 `[file] Read` |
| VRAM im Normallauf | 0 Writes |

### Referenz

* Emulator: Azahar 2126.2 (`%APPDATA%\azahar`), laeuft mit dem ROM.
* Referenzbild des gerenderten Titel-/Menue-Screens:
  `reports/azahar_topscreen.png`, `reports/azahar_bottomscreen.png`
  (PrintWindow-Capture, 28,37 % gesaettigte Pixel, teal Hintergrund, helles Logoband).

## Werkzeuge / Schalter (Auswahl)

* `HLE_GPU_TEST=1` - synthetische Liste ausfuehren (Selbsttest)
* `HLE_GPU_TEST_ADDR=<hex>`, `HLE_GPU_TEST_SIZE=<hex>` - beliebigen Bereich als Liste ausfuehren
* `HLE_GPU_SUBMIT=1` - `0x0006` + `0x000C` ueber die echten Handler abgeben
* `HLE_FRAME_LOOP=<hex>`, `HLE_FRAME_ARG=r0,r1,r2,r3` - Gastfunktion periodisch aufrufen
* `HLE_FORCE_FRAME=<hex>`, `HLE_FORCE_ARG=...` - einmaliger Aufruf
* `HLE_VB_ONLY`, `HLE_KEEP_SLOT`, `HLE_REG_ENABLE`, `HLE_FS_PRESET`, `HLE_FRAME_CB`
* `ARM_WATCH`, `ARM_MEMWATCH`, `ARM_MEMWATCHRD`, `ARM_MEMWATCHRANGE`,
  `HLE_MEMWATCHVAL`, `HLE_DUMP_RANGE`, `HLE_DUMP_REG`, `HLE_BLKLOGF`
* `HLE_ASSET_TRACE`, `HLE_FA_DEBUG`, `HLE_THR_LOG`, `HLE_WAIT_LOG`

**Wichtig:** Bei Adressen >= `0x10000000` schreibt der Memwatch `mw] 1F...`
(nicht `mw] 001F...`).

## Build / Test

```
python tools\build_port.py      -> out/port.exe
python tests\mvp_selftest.py    -> 13/13
.\out\port.exe out\romfs --shot out\shot.bmp
```

## Offene Aufgabe

Der Spielzustand, der den Render-Task (und damit die PICA-Liste + Trigger)
erzeugt, ist ohne Emulator-Trace nicht bestimmt. Naechste Ansaetze:

1. GSP-Shmem `0x10002000` + `ARM_WATCH=13E3E0` gemeinsam beobachten (bisher:
   nur 1 Nullwort, Trigger nie).
2. Erzeuger des Render-Tasks suchen (Task-Tabelle im Umfeld `0x0040F038`).
3. FA-/Szenen-Phase: `arc/lt6_a.fa` muesste geoeffnet und der Archivindex
   aufgebaut werden (FA-Resolver `0x36C54C`/`0x2B95F8` funktioniert bereits).
