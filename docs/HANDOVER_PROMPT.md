# Übergabe-Prompt: Layton-6-Port — echtes Hauptmenü (oben + unten)

Kopiere den Text ab `AUFTRAG` in eine neue Session.

---

AUFTRAG / GOAL (max. 5 Sätze)
Der Port zeigt aktuell nur den oberen Screen als vorgerenderten Referenz-Screenshot; dieser Weg wird nicht weiterverfolgt, die Referenzframes dienen nur noch dem Pixelvergleich. Ziel ist, das echte Hauptmenü vollständig von der Spiel-Engine rendern zu lassen, und zwar oberen (400x240) und unteren (320x240) Screen übereinander in einem 400x480-Fenster. Die Engine muss ihre Szene selbst bauen und über `gsp::Gpu` (`SubmitCmdList`/`SetCommandList`, `TriggerCmdReqQueue`, `SetBufferSwap screen=0/1`) in beide Framebuffer schreiben; kein Host-`memcpy` und kein Referenz-Blit als Renderquelle. Eingaben sind noch nicht nötig, Sound ist optional. Jede Behauptung ist mit Log, Testlauf oder Dateiinhalt zu belegen; Sub-Agents und Skills dürfen genutzt werden.

AUSGANGSLAGE
- `python tests\mvp_selftest.py` → 13/13; `out/` ist aufgeräumt (Historie in `out/logs/`).
- Top-Screen läuft über den gsp-Pfad (`SubmitCmdList`-Ring, `0x0006`, `0x000C`, `0x0005`), Quelle ist aber noch `reports/menu_official.bin` (Referenz-Blit).
- Engine-Draw-Liste wächst: `HLE_DRAW_LIST=1 HLE_DRAW_FIX=1` → `[dl] count=11` (Objekt `0x003EE12C`); vorher Abbruch bei `0x280338` wegen leerer Pool-Slots (in `hle.c` gefixt).
- Bottom-Referenz `reports/menu_official_bottom.bin` (320x240) dient nur dem Vergleich.

DATEIEN
- `tests/port.c` — Fenster/FB (aktuell 400x240) auf 400x480; Top y=0, Bottom y=240, x=40.
- `runtime/hle/hle.c` — `hle_menu_gpu_submit`, `gsp_process_cmdqueue`, `g_gsp_fb_addr[3]` (static → Getter).
- `runtime/gpu.c` — globaler `g_fb`, `BlitRGB` 0x07.
- `runtime/arm_trace.c` — env-gated Sonden.
- `tools/_make_menu_frame.py`, `tools/_cmp_shot.py`.

PLAN
1. Fenster 400x480; Top aus `g_gsp_fb_addr[0]` (Default `0x1F000000`), Bottom aus `[1]`; vor jedem Screen `gpu_set_fb()` setzen.
2. `HLE_MENU_GPU` im finalen Lauf aus; die Engine muss beide FBs über `SetBufferSwap screen=0/1` füllen.
3. Gates wahr machen: `[0x3EE16C]` (`scene+0x2144`), `[0x40B4CC]` (`0x00101750`), `[0x40B354]`, `[0x3D8D5D]`, `[obj+0x6A]`, `[obj+0x74]`, `[renderer+0x15A3]`, `[0x40B4E0]` (aktuell `bx lr`).
4. Engine-Submit: `fn_001196AC` → `fn_00120CFC` → `fn_00275B24` (0x0005); `fn_00120DA0` → `fn_0027CBDC` → `fn_001263B8` (0x000C); `fn_001B9938` → `fn_0034AFAC` → `fn_0034B97C` (0x0006).
5. Die 11 Draw-Einträge in `gpu.c`-Kommandos übersetzen oder den PICA-Stream der Engine ausführen; danach Top/Bottom vergleichen.

BUILD / TEST
```powershell
python tools\build_port.py
python tools\build_boot.py --out out\boot.exe
.\out\port.exe out\romfs --shot out\shot.bmp
python tools\_cmp_shot.py out\shot.bmp reports\menu_official.bin
```
`tools/decomp.py` überschreibt `code_decomp.bin`; danach `apply_all_patches.py`,
`patch_real_loader.py`, `patch_frame_callback.py`, `build_port.py`.

FALLEN
- `arm_trace_block` nur an Blockanfängen; `getenv()` in `static` cachen.
- Läufe langsam (Boot bis `.fa` 120–300 s); `HLE_KEEP_ALIVE`/`PORT_SHOT_FRAMES` großzügig.
- `out/romfs_image.bin` pristine lassen; keine neuen Ketten-Patches.
- `HLE_MENU_GPU`/Referenz-Blit sind nur Debug-Fallback, nicht Abnahme.
- `g_gsp_fb_addr` ist `static`; GPU hat nur einen globalen `g_fb`.

ABNAHME
1. `out\port.exe out\romfs` zeigt ein 400x480-Fenster mit **beiden** Screens, ohne Eingaben.
2. Im Log: `SetBufferSwap screen=0` und `screen=1`, `SubmitCmdList`/`0x0006`/`0x000C`; VRAM-Writes in beiden FB-Adressen.
3. Screenshot `--shot` + Pixelvergleich beider Regionen; `mvp_selftest.py` 13/13.
4. Beleg-Abschnitt in `reports/PORT.md` und `docs/PORT_STATE.md`.

Details/Historie: [`docs/PORT_STATE.md`](PORT_STATE.md), [`reports/PORT.md`](../reports/PORT.md).
