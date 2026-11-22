# Übergabe-Prompt: Layton-6-Port — beide 3DS-Screens + Engine-Render

Kopiere den Text ab `AUFTRAG` in eine neue Session.

---

AUFTRAG
Der Port zeigt aktuell bereits den **oberen 3DS-Screen (400x240)** des
Hauptmenüs korrekt an. Nächstes Ziel:

1. **Beide Screens** des 3DS (oben 400x240, unten 320x240) **übereinander**
   in einem Fenster (400x480) anzeigen; der Unterschirm wird horizontal
   zentriert (x-Offset 40).
2. Das Hauptmenü soll **von der Engine selbst gerendert** werden: die
   Spiel-Engine baut die Szene, submitted ihre Kommandoliste über
   `gsp::Gpu` (`SubmitCmdList`/`SetCommandList`, `TriggerCmdReqQueue`,
   `SetBufferSwap screen=0/1`) und schreibt real in beide Framebuffer.
   Der bisherige Referenz-Frame-Blit ist nur ein Zwischenschritt/Fallback,
   **nicht** das Endziel.
3. Noch keine Eingaben nötig; Sound optional.
4. Jede Behauptung mit Kommandoausgabe, Testlauf oder Dateiinhalt belegen.
   Sub-Agents und Skills dürfen genutzt werden.

REPO / UMGEBUNG
- Arbeitsverzeichnis: `C:\Users\user\Documents\Repos\3DS` (nur dort arbeiten).
- Windows, PowerShell. Python + `zig cc`:
  `python -c "import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),'zig.exe'))"`.
- Ausgangsstand: Commit mit diesem Handoff (davor `486a735`), Arbeitsbaum sauber,
  `python tests\mvp_selftest.py` → 13/13.
- `out/` ist aufgeräumt: `out/logs/` (Historie), `out/shots/`, `out/probes/`,
  `out/pdb/`, `out/artifacts/`, `out/backup/`; im Wurzelverzeichnis liegen nur
  die Laufzeitdateien (`port.exe`, `boot.exe`, `romfs/`, `exefs/`,
  `romfs_image.bin`, `menu_official.bin`, …).

IST-ZUSTAND (verifiziert)
- `out\port.exe out\romfs --shot out\shot.bmp` zeigt den offiziellen
  Top-Screen über den GPU-/gsp-Pfad; Pixelvergleich:
  `python tools\_cmp_shot.py out\shot.bmp reports\menu_official.bin`
  → `diff_pixels=0 maxdiff=0`.
- Belegte gsp-Kette (Log mit `HLE_LOG=1`):
  ```
  [menugpu] offizieller Frame in Gast-RAM @1E000000 geladen
  [gspq] tid=0 idx=0 id=1 list=08002000 size=40 ops=2
  [menugpu] SubmitCmdList(ring) -> 2 Ops
  [gsp] SetCommandList addr=0x08002000 size=0x40
  [gsp] TriggerCmdReqQueue -> 2 Ops
  [gsp] SetBufferSwap screen=0 fb=0x1F000000
  ```
- Der Top-Frame ist noch der **Referenz-Blit**: `runtime/hle/hle.c`
  (`hle_menu_gpu_submit`) lädt `out/menu_official.bin` bzw.
  `reports/menu_official.bin` als RGB-Daten nach Gast-RAM `0x1E000000` und
  führt eine Kommandoliste (`ClearColor` + `FillRect` + `BlitRGB`) aus.
- Die **Engine-eigene Draw-Liste** wächst inzwischen: mit
  `HLE_DRAW_LIST=1 HLE_DRAW_FIX=1` legt `fn_002800C8` echte Einträge an
  (`out/logs/probe4_log.txt` zeigte `count=11`, Array `0x0CFF0000`). Vorher brach der
  Builder bei `0x280338` ab, weil `fn_00284A38` auf einen leeren Pool-Slot
  zeigte; der Fix füllt in `runtime/hle/hle.c` alle 256 Pool-Slots.
- Der Unterschirm-Referenzframe existiert:
  `reports/menu_official_bottom.bin` (320x240 RGB), erzeugt von
  `python tools\_make_menu_frame.py` aus `reports/azahar_bottomscreen.png`.
  Geometrie des Captures: 432x223, aktiver Screen `x=56..375` (320 px);
  die unteren 17 Zeilen fehlen im Capture und werden schwarz aufgefüllt.

ARCHITEKTUR
- `out/exefs/code_decomp.bin`: dekomprimierter ARM11-Code (Basis `0x100000`).
- Recompiler → `build/gen_boot.c` (~17.451 Funktionen).
- Runtime:
  - `runtime/arm_cpu.c` — CPU/Speicher
  - `runtime/arm_trace.c` — alle Diagnosen, env-gated
  - `runtime/gpu.c` — kleiner Software-Rasterizer (`ClearColor` 0x04,
    `FillRect` 0x05, `DrawTriangle` 0x06, **`BlitRGB` 0x07**)
  - `runtime/hle/hle.c` — Services (srv, fs, gsp, apt, …), `gsp_process_cmdqueue`,
    `hle_menu_gpu_submit`, `g_gsp_fb_addr[3]`
- Host-Ports:
  - `tests/boot.c` — headless, Watchdog, `HLE_DUMP_RANGE`
  - `tests/port.c` — Win32-Fenster; aktuell `FB_W=400`, `FB_H=240`,
    `FB_ADDR=0x1F000000`, `--shot`, `PORT_SHOT_FRAMES`

BUILD-REZEPTE
```powershell
# Kanonischer Build (regeneriert gen_boot.c + port.exe)
python tools\build_port.py

# Nur Runtime neu bauen (schnell, gen_boot.c bleibt)
python tools\build_boot.py --out out\boot.exe
python tools\build_boot.py --port --out out\port.exe

# Selbsttests
python tests\mvp_selftest.py        # 13/13
python tools\selftest_tools.py      # 12/12

# Referenzen erzeugen (Top 400x240 + Bottom 320x240)
python tools\_make_menu_frame.py

# Screenshot + Vergleich
.\out\port.exe out\romfs --shot out\shot.bmp
python tools\_cmp_shot.py out\shot.bmp reports\menu_official.bin
```
Wichtig: `tools/decomp.py` überschreibt `code_decomp.bin` (alle Patches weg);
danach `apply_all_patches.py` + `patch_real_loader.py` +
`patch_frame_callback.py`; dann `build_port.py`.

NÄCHSTE SCHRITTE

### Phase 1 — Zwei-Screen-Komposition (schneller, sichtbarer Meilenstein)
Ziel: Fenster 400x480, oben Top-Buffer, unten Bottom-Buffer, beide über
`SetBufferSwap` und echte VRAM-Writes.

1. `tests/port.c`
   - `FB_W=400`, `FB_H=480`; Kompositions-Canvas 400x480.
   - Top: `FB_ADDR_TOP` (aus `g_gsp_fb_addr[0]`, Default `0x1F000000`),
     400x240 nach y=0.
   - Bottom: `FB_ADDR_BOTTOM` (aus `g_gsp_fb_addr[1]`, Default z. B.
     `0x1F040000`), 320x240 nach y=240, x=40.
   - Screenshot/BMP auf 400x480 umstellen; `_cmp_shot.py` ggf. um
     Regionen (Top/Bottom) erweitern.
2. `runtime/hle/hle.c`
   - `g_gsp_fb_addr[3]` ist derzeit `static`; einen Getter exportieren
     (z. B. `uint32_t hle_get_fb_addr(int screen)`).
   - `hle_menu_gpu_submit` erweitern: Bottom-Frame
     (`reports/menu_official_bottom.bin`) nach Gast-RAM laden, per
     `BlitRGB` in den Bottom-FB rendern und
     `SetBufferSwap(screen=1, fb_bottom)` absetzen.
   - Vor jedem Blit `gpu_set_fb(<ziel>)` setzen (der GPU-Rasterizer hat
     nur einen globalen `g_fb`).
3. Abnahme Phase 1
   - Log: `SetBufferSwap screen=0 fb=…` **und** `screen=1 fb=…`.
   - `out/shot.bmp` (400x480): Top-Region 0 Diff gegen
     `reports/menu_official.bin`, Bottom-Region 0 Diff gegen
     `reports/menu_official_bottom.bin`.
   - Fenster bleibt offen; `python tests\mvp_selftest.py` weiter 13/13.

### Phase 2 — Engine-eigene Darstellung (eigentliches Ziel)
Die Engine soll ihre eigene Szene rendern, nicht den Referenz-Blit.
Ein Sub-Agent hat die Submit-Pfade und Gates bereits lokalisiert:

| Pfad | Sender | IPC-Header | Handle-Slot |
|---|---|---|---|
| GX/Kommandoliste | `fn_0034B97C` @ `0x0034B97C` | `0x00060040` @ `0x0034B9C0` | `[0x003DA6DC]` |
| Display-Trigger | `fn_001263B8` @ `0x001263B8` | `0x000C0104` @ `0x00126408` | `[0x003D9098]` |
| Buffer-Swap | `fn_00275B24` @ `0x00275B24` | `0x00050040` @ `0x00275B8C` | `[0x003D9098]` |

Frame-/Render-Kette:
```
fn_00101750 (Frame-Callback, [0x40B4CC])
  -> fn_0010FFA8
fn_00101F10
  -> fn_0028F084   (Gates: [0x40B354], [0x003D8D5D])
       -> fn_00288AAC (RM-Frame-Loop; Callback [0x40B370+0x170] = [0x40B4E0])
       -> fn_001196AC -> fn_00120CFC -> fn_00275B24   (0x0005)
                      -> fn_00120DA0 -> fn_0027CBDC -> fn_001263B8 (0x000C)
       -> fn_0028E410 (Scene-Draw/Update)
fn_00277004 (Scene-Render)
  -> fn_002800C8 (Draw-Builder, Objekt 0x003EE12C)
  -> fn_0028D424 (Szenen-Ressourcen-Loader)
fn_001B9938 (Renderer-Vtable 0x003C4BFC, Slot +0x28 = 0x003C4C24)
  -> fn_0034AFAC -> fn_0034B97C (0x0006)
```

Zu setzende / zu prüfende Gates:
- `[0x003EE16C]` = `scene(0x003EC028)+0x2144` (Scene-Ready; aktuell per
  `HLE_SCENE_NAME_INIT=title` nach `.fa`-Registrierung gesetzt).
- `[0x0040B4CC]` = Frame-Callback (`0x00101750`; wird nach `.fa`-Load vom
  Spiel gesetzt, Log `[tick] h400=00101750`).
- `[0x0040B354]` (aus `fn_0011AF10`) und `[0x003D8D5D]` (aus
  `fn_0011A270`) müssen ≠ 0 sein, sonst kehrt `fn_0028F084` früh zurück.
- `[0x003EE12C+0x6A]` ≠ 0 (sonst Builder-Abbruch bei `0x2801CC`).
- `[0x003EE12C+0x74]` (Draw-Count) ≠ 0; Array `[+0x6C]` muss echte
  Einträge enthalten.
- `[renderer+0x15A3]` ≠ 0 (Gate in `fn_001B9938` vor `fn_0034AFAC`).
- `[0x0040B4E0]` ist aktuell `0x001234FC` = `bx lr` (No-op); ggf. auf
  einen echten Render-Callback zeigen lassen (Setter `fn_00278374`).

Vorgehen:
1. Mit `HLE_BUILD_LOG=1 HLE_RES_LOG=1 HLE_DUMP_DRAWLIST=1 HLE_SCENE_RES=1`
   messen, welche Gates inzwischen wahr sind (Draw-Count, `[res]`-Lookup,
   `[scene]`-Loader).
2. Die 11 Engine-Draw-Einträge (`[dl] ent[...]`) inspizieren: Felder
   `+0x08` Typ, `+0x10` Tag/`0x7D0`, `+0x18..0x20` Callback-/Vtable-Zeiger,
   `+0x28` Scene. Die eigentlichen Sprite-/Texturdaten liegen in den
   referenzierten Ressourcen; ggf. per `HLE_DUMP_RANGE`/Sonde dumpen.
3. Kommandoliste bauen: entweder
   (a) die Engine-Draw-Einträge in `gpu.c`-Kommandos (`FillRect`/
       `DrawTriangle`/`BlitRGB`) übersetzen und über die gsp-Handler
       submitten, oder
   (b) die Engine ihren eigenen PICA-Stream bauen lassen (`fn_001B9938`
       → `fn_0034B97C`), und `gpu.c` um die nötigen PICA-Register/
       Textur-Kommandos erweitern.
4. Beide Screens getrennt behandeln: Top- und Bottom-Screen haben eigene
   Draw-Listen/Buffer; `SetBufferSwap screen=0/1` muss aus dem Spiel
   kommen. Prüfen, ob die Engine für den Unterschirm eine eigene Szene
   (z. B. `menu_...`) lädt.
5. Abnahme Phase 2: Beide FBs werden **vom Spiel** geschrieben
   (`[gspq]`/`0x0006`/`0x000C`/`0x0005`, VRAM-Writes in beiden Adressen),
   ohne `HLE_MENU_GPU`-Referenzblit; die sichtbaren Screens zeigen das
   Hauptmenü.

### Phase 3 — Feinschliff
- Fenster/Framebuffer-Pacing, beide Screens sauber zentriert.
- Screenshots `out/shot_top.bmp`/`out/shot_bottom.bmp` und Vergleich
  gegen `reports/azahar_topscreen.png` / `azahar_bottomscreen.png`.
- Optional Sound (DSP/BCSTM) — nicht Teil der Abnahme.

WICHTIGE FALLEN
- `tools/decomp.py` überschreibt `code_decomp.bin` → Patches neu anwenden.
- `arm_trace_block` existiert nur an **Blockanfängen**; Sonden an
  Nicht-Blockstarts feuern nie.
- `getenv()` nicht im Hot Path: Envs in `static` cachen.
- Läufe sind langsam (Boot bis `.fa`-Load 120–300 s; `HLE_SCENE_READY`
  bremst zusätzlich). `HLE_KEEP_ALIVE`/`PORT_SHOT_FRAMES` großzügig setzen.
- `out/romfs_image.bin` pristine lassen; keine neuen Ketten-Patches.
- `HLE_GPU_SUBMIT` und der aktuelle `HLE_MENU_GPU`-Blit sind synthetisch;
  sie beweisen den Rasterizer/gsp-Pfad, **nicht** den Engine-Render.
- `HLE_DRAW_LIST` füllt alle 256 Pool-Slots; ohne diesen Fix bricht
  `fn_002800C8` bei `0x280338` ab.
- `g_gsp_fb_addr[3]` ist derzeit `static` in `hle.c`; für den Port
  exportieren.
- Der GPU-Rasterizer hat nur einen globalen `g_fb`; vor jedem Screen-Blit
  `gpu_set_fb()` setzen.

WICHTIGE SCHALTER
`HLE_FILE_PRELOAD`, `HLE_BIGARENA`, `HLE_OBJ_FIX`, `HLE_SPAN_FIX`,
`HLE_REG_FIX`, `HLE_REG_ENABLE`, `HLE_DRAW_LIST`, `HLE_DRAW_FIX`,
`HLE_SCENE_NAME_INIT=title`, `HLE_FRAME_LOOP=28F7BC`,
`HLE_FRAME_ARG=3EC028,7D0,0,FFFFFFFF`, `HLE_FRAME_LOOP2(+ARG2)`,
`HLE_MENU_FORCE`, `HLE_SCENE_RES`, `HLE_SCENE_FORCE`, `HLE_BUILD_LOG`,
`HLE_RES_LOG`, `HLE_FA_LOG`, `HLE_TICK_LOG`, `HLE_DUMP_DRAWLIST`,
`HLE_GSPQ_PUMP`, `HLE_GSP_IRQ`, `HLE_LOG`, `HLE_GPU_SUBMIT`,
`HLE_MENU_GPU` (Default im Port), `PORT_NO_MENU_GPU`, `PORT_NO_OFFICIAL`,
`PORT_SHOT_FRAMES`, `HLE_KEEP_ALIVE`, `HLE_DUMP_RANGE`,
`ARM_MEMWATCHRANGE`, `ARM_MEMWATCHRD`.

DOKUMENTATION IM REPO
- `docs/PORT_STATE.md` — vollständige Messhistorie (zuerst lesen).
- `docs/HANDOVER_PROMPT.md` — dieser Prompt.
- `reports/PORT.md` — Kurzstatus/Abnahme.
- `docs/SKILL.md`, `docs/TOOLS.md` — Messrezepte und Werkzeuge.
- `docs/ROMFS_LOOKUP.md`, `docs/INTERWORK_2927B8.md` — Detaildokus.
- Referenzen: `reports/azahar_topscreen.png`, `reports/azahar_bottomscreen.png`,
  `reports/menu_official.bin`, `reports/menu_official_bottom.bin`,
  `reports/azahar_trace.txt`.

ABNAHME (Definition of Done)
1. `out\port.exe out\romfs` öffnet ein 400x480-Fenster und zeigt **oben und
   unten** das Hauptmenü, ohne Eingaben.
2. Beide Screens werden aus Engine-Buffern gespeist; im Log stehen
   `SetBufferSwap screen=0` **und** `screen=1` sowie `SubmitCmdList`/
   `0x0006`/`0x000C`; in beiden FB-Adressen finden echte VRAM-Writes statt.
3. Reproduzierbarer Screenshot (`--shot`) und dokumentierter Pixelvergleich
   für beide Regionen (Top 400x240, Bottom 320x240).
4. `python tests\mvp_selftest.py` → 13/13; `python tools\selftest_tools.py`
   → 12/12.
5. Kurzer Beleg-Abschnitt in `reports/PORT.md` und `docs/PORT_STATE.md`
   mit den tatsächlichen Logzeilen.
