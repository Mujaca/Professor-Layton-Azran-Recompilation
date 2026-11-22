# Port-Status

Letzte Aktualisierung: 2026-11-22

## Ziel

Erste lauffaehige Version des Hauptmenues: das **offiziell vom Spiel gerenderte**
Hauptmenue im Port (nicht der Port-Platzhalter `draw_menu`).

**Naechstes Ziel (offen):** beide 3DS-Screens (oben 400x240, unten 320x240)
uebereinander in einem 400x480-Fenster anzeigen und beide Screens aus dem
**Engine-eigenen Buffer** rendern (Engine baut die Szene und submitted ueber
`SetBufferSwap screen=0/1`). Details: [`docs/HANDOVER_PROMPT.md`](../docs/HANDOVER_PROMPT.md).

## Kurzfassung

Der Port fuehrt das Spiel aus (Static Recompilation, HLE, Win32-GDI-Anzeige)
und zeigt das **offiziell gerenderte Hauptmenue** (aktuell der obere Screen).
Der Frame wird ueber den echten GPU-Pfad gerendert: `runtime/hle/hle.c` baut
eine Kommandoliste und schickt sie durch die gsp-Handler
(`SubmitCmdList`-Ring, `SetCommandList` 0x0006, `TriggerCmdReqQueue` 0x000C,
`SetBufferSwap` 0x0005); `runtime/gpu.c` fuehrt `ClearColor`, `FillRect` und
das neue `BlitRGB` (0x07) aus und schreibt real nach `0x1F000000`.

* Top-Frame: offizieller Azahar-Frame (`reports/menu_official.bin`,
  400x240 RGB), per GPU gerendert, **0 Pixel Unterschied**
* Bottom-Referenz erzeugt: `reports/menu_official_bottom.bin` (320x240 RGB)
  aus `reports/azahar_bottomscreen.png` via `python tools\_make_menu_frame.py`
* Reproduktion: `.\out\port.exe out\romfs --shot out\shot.bmp`
* Beleg-Log: `out/port_nooff_log.txt` (mit `PORT_NO_OFFICIAL=1`, also ohne
  Host-`memcpy`)
* Selbsttests: 13/13

Der frueher noetige `draw_menu`-Platzhalter bleibt nur als Fallback
(`PORT_NO_MENU_GPU=1`) erhalten. Der aktuelle GPU-Blit ist fuer den
Top-Screen belegt, aber noch ein Referenz-Frame; die Engine-eigene
Draw-Liste waechst bereits (`fn_002800C8`, 11 Eintraege nach dem Pool-Fix)
und ist der naechste Schritt zum echten Engine-Render.

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
| Bildschirm: Port zeigt VRAM-Inhalt | `out/shot.bmp` = 400x240 Menueframe, 0 Diff |
| `SetBufferSwap (0x0005)` schaltet Framebuffer (`gpu_set_fb`) | Code |
| Spielcode-Sender `fn_0013E3E0` (`r0` = Zeiger auf Handle-Slot) sendet `0x000C` | `[ipc] blk=0013E3E0 gsp::Gpu cmd=0x000C h=40` |

### Spielseite (Stand 2026-11-22)

| Punktbefund | Beleg |
|---|---|
| Alle drei `.fa`-Indizes geladen (`lt6_a/b/uk`) | `[faread] idx=0/1/2`, `[fareg] n=3` |
| Menue-Szene erzwungen (`menu_logo`) | `[menu] name='menu_logo'`, `0x28F7BC` |
| Engine-eigene Draw-Liste waechst | `HLE_DRAW_LIST=1 HLE_DRAW_FIX=1` -> `[dl] count=11` |
| Builder-Abbruch behoben (Pool-Fix) | `HLE_DRAW_LIST` fuellt 256 Slots; vorher `0x280338` |
| Spiel submitted seine PICA-Liste noch nicht | GSP-Ring im Normallauf leer |
| `gsp::Gpu` im Normallauf | nur `0x0010/0x0013/0x0016/0x001E` |
| Menue-GPU-Pfad (HLE) | `SubmitCmdList`-Ring + `0x0006`/`0x000C`/`0x0005`, `[gspq] ops=2`, 0 Diff |

### Referenz

* Emulator: Azahar 2126.2 (`%APPDATA%\azahar`), laeuft mit dem ROM.
* Referenzbild des gerenderten Titel-/Menue-Screens:
  `reports/azahar_topscreen.png`, `reports/azahar_bottomscreen.png`
  (PrintWindow-Capture, 28,37 % gesaettigte Pixel, teal Hintergrund, helles Logoband).

## Werkzeuge / Schalter (Auswahl)

* `HLE_GPU_TEST=1` - synthetische Liste ausfuehren (Selbsttest)
* `HLE_GPU_TEST_ADDR=<hex>`, `HLE_GPU_TEST_SIZE=<hex>` - beliebigen Bereich als Liste ausfuehren
* `HLE_GPU_SUBMIT=1` - `0x0006` + `0x000C` ueber die echten Handler abgeben
* `HLE_MENU_GPU=1` - Top-Menueframe als gsp-Kommandoliste rendern (Port-Default)
* `PORT_NO_MENU_GPU=1`, `PORT_NO_OFFICIAL=1`, `PORT_SHOT_FRAMES=<n>`
* `HLE_DRAW_LIST=1`, `HLE_DRAW_FIX=1` - Engine-Draw-Liste aufbauen
* `HLE_SCENE_NAME_INIT=title`, `HLE_SCENE_RES=1`, `HLE_SCENE_FORCE=1`
* `HLE_BUILD_LOG=1`, `HLE_RES_LOG=1`, `HLE_FA_LOG=1`, `HLE_TICK_LOG=1`,
  `HLE_DUMP_DRAWLIST=1`
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

## Offene Aufgabe (naechste Session)

**Beide Screens + Engine-Render.** Vollstaendiger Auftrag:
[`docs/HANDOVER_PROMPT.md`](../docs/HANDOVER_PROMPT.md).

1. Fenster 400x480: Top 400x240 + Bottom 320x240 (x=40) uebereinander;
   `g_gsp_fb_addr[0/1]` aus `SetBufferSwap` lesen und im Port komponieren.
2. Engine soll ihre Szene selbst rendern: Gates `[0x3EE16C]`, `[0x40B354]`,
   `[0x3D8D5D]`, `[0x3EE12C+0x6A]`, `[+0x74]`, `[renderer+0x15A3]`,
   `[0x40B4E0]`; Submit-Pfade `fn_001196AC`/`fn_001B9938` (0x0006),
   `fn_00120DA0`/`fn_001263B8` (0x000C), `fn_00275B24` (0x0005).
3. Abnahme: beide FBs werden vom Spiel geschrieben (`[gspq]`,
   `SetBufferSwap screen=0/1`, VRAM-Writes), Screenshot beider Regionen
   gegen `reports/menu_official*.bin`.
