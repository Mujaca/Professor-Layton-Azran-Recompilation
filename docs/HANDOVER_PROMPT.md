# Übergabe-Prompt: Layton-6-Port (3DS → PC, statische Rekompilation)

Kopiere den Text ab „AUFTRAG" in eine neue Session.

---

AUFTRAG
Bringe eine erste lauffähige Version des Hauptmenüs von *Professor Layton and the
Azran Legacy* (3DS, EUR) als natives PC-Programm zum Laufen. Als „erste
lauffähige Version" zählt ausschließlich das **offiziell vom Spiel gerenderte
Hauptmenü** (keine Eingaben nötig, Sound optional). Ein selbstgezeichneter
Platzhalter zählt NICHT. Arbeite autonom weiter; Sub-Agents und Skills dürfen
genutzt werden. Jede Behauptung muss mit Kommandoausgabe/Testlauf/Dateiinhalt
belegt werden.

REPO / UMGEBUNG
- Arbeitsverzeichnis: `C:\Users\user\Documents\Repos\3DS` (nur dort arbeiten;
  Build-Tools außerhalb sind erlaubt).
- Windows, PowerShell. Python + `zig cc` (Pfad via
  `python -c "import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),'zig.exe'))"`).
- Git: HEAD `1716727`, Arbeitsbaum sauber, `python tests\mvp_selftest.py` → 13/13.

ARCHITEKTUR
- `out/exefs/code_decomp.bin`: dekomprimierter ARM11-Code (Basis `0x100000`),
  von `tools/decomp.py` aus `out/exefs/code.bin` erzeugt.
- Recompiler → `build/gen_boot.c` (~17.452 Funktionen).
- Runtime: `runtime/arm_cpu.c` (CPU/Speicher), `runtime/arm_trace.c` (alle
  Diagnosen, env-gated), `runtime/gpu.c` (kleiner Software-Rasterizer für
  PICA-Kommandolisten), `runtime/hle/hle.c` (Services: srv, fs, gsp, apt, …).
- Host-Ports: `tests/boot.c` (headless, Watchdog, `HLE_DUMP_RANGE`) und
  `tests/port.c` (Win32-Fenster, FB `0x1F000000`, `--shot`, `PORT_SHOT_FRAMES`).

BUILD-REZEPTE
```
python tools\decomp.py                  # ACHTUNG: überschreibt alle Patches!
python tools\apply_all_patches.py       # dokumentierter Patch-Satz (feste Reihenfolge)
python tools\patch_real_loader.py       # echter Mount ohne 0x1249CC-Short-Circuit
python tools\patch_frame_callback.py    # NOP an 0x11BA98 (Frame-Callback-Registrierung)
python tools\build_port.py              # regeneriert build/gen_boot.c + out/port.exe
zig cc -std=c11 -O1 -I runtime -I runtime\hle tests\boot.c build\gen_boot.c ^
  runtime\arm_cpu.c runtime\arm_trace.c runtime\gpu.c runtime\hle\hle.c -o out\boot2.exe
```
Merksatz: **Jede** Änderung an `out/exefs/code_decomp.bin` erfordert
`tools\build_port.py` und einen Neubau von `boot.exe`/`boot2.exe`.

BEREITS BELEGT (mit Ausgaben)
- RomFS: `out/romfs_image.bin` (951.353.344 B) aus dem ROM
  (`tools/romfs_image.py`); HLE zieht beim Read `+0x1000` (IVFC) ab.
- Lookup trifft; BCSAR wird vorgeladen:
  `[preload] size=00F3DE78 dataOff=26B93A40 dst=18000000 got=15982200`.
- Mount-Erfolg **ohne** Short-Circuit (mit apply_all + patch_real_loader +
  `HLE_OBJ_FIX=1 HLE_SPAN_FIX=1 HLE_REG_FIX=1`):
  `[chain] pc=00124C30 r0=00000001` → `[romfs] 002850B8 r0=1`, `00285184`.
- Menü-Szene: über `HLE_FRAME_LOOP=28F7BC` + `HLE_FRAME_ARG=3EC028,7D0,0,FFFFFFFF`
  läuft der echte Menü-Zweig: `0x28FCE8` (menu_logo) → `0x28A164`
  (Name `menu_logo`) → `0x28A1BC` → `0x1117A4` → `0x2800C8`
  (441 Draw-Objekte). Zustand `r4=0x7D0`.
- Frame-Dispatcher `0x134818`/`0x134854` läuft und zählt Frames
  (`[ctx+0x154]/[ctx+0x158]`, Watchdog: 1,2,3,…).
- Screenshot-Timing war lange ein Artefakt: `--shot` speicherte bei Frame 3.
  Neu: `PORT_SHOT_FRAMES=<n>`. Mit synthetischem `HLE_GPU_SUBMIT=1` und
  `PORT_SHOT_FRAMES=200` → `shot_late.bmp: 288054 B, Unterschiede: 284003`
  (beweist: VRAM-Pixel landen korrekt im Screenshot).

OFFENER HAUPTBLOCKER
Der Frame-Callback `[0x40B370+0x15C]` = `[0x40B4CC]` bleibt **0**. Dadurch ruft
der laufende Dispatcher pro Frame keine Arbeit auf → kein `SubmitCmdList` im
GSP-Kommandopuffer `0x10002800` → kein `gsp 0x000C`/`0x0005` → keine
VRAM-Writes des Spiels → Screenshot bleibt byte-identisch zum Platzhalter
(288.054 B, 0 Differenzen).

Bekannte Fakten dazu:
- `0x134818`: `ldr r4,[pc]=0x0040B370; ldr r1,[r4,#0x15c]; movne r0,#0x400;
  blxne r1` — sonst nur Zähler++.
- Kontext `0x40B370` ist eine Struktur mit Bildschirmmaßen
  (`0x40B370=0xF0`, `+4=0xF0`, `+8=0x190`, `+0xC=0x140`) und Code-Zeigern
  (`0x40B4E0=0x001234FC`, gesetzt von `0x278374` via `0x11C458`, Literal
  `0x40B370` @`0x278388`).
- `tools/patch_frame_callback.py` dokumentiert den Registrar:
  `fn_0011B98C`: `0x11BA8C bl 0x12711C`; bei 0 → Registrierung überspringen;
  sonst `0x11BA9C ldr r1,[r6,#0xF8]` (=`0x00101750`) und
  `0x11BAA4 bl 0x27A8A0` mit `r0=0x400` → setzt `[0x40B4CC]`.
  Patch (NOP an `0x11BA98`) ist aktuell **angewendet**, aber `0x40B4CC` bleibt
  0 → `fn_0011B98C` wird im aktuellen Ablauf gar nicht erreicht.
- Main-Thread-Kette (`HLE_CALL_LOG`, armiert an `0x2850B8`): … `0x1288xx` →
  `0x134144` → `0x134154` → `0x13CC30` / `0x1413C4` / `0x141438` / `0x141448`
  (Log-Cap 160 erreicht). Der Abbruch liegt also **später** als `0x101508`.

NÄCHSTE SCHRITTE (in dieser Reihenfolge)
1. In `runtime/arm_trace.c` den Cap von `HLE_CALL_LOG` (160) auf ~1500 erhöhen,
   `HLE_CALL_LOG=1` + `HLE_FILE_PRELOAD=1` + `HLE_BIGARENA=1` laufen lassen und
   den **letzten** neuen Block des Main-Threads bestimmen.
2. Sonde an `0x11B98C` (und `0x11BA90`) setzen: wird `fn_0011B98C` erreicht und
   was liefert `0x12711C`? Wenn nicht erreicht: Aufrufer über Callers-Liste
   zurückverfolgen und den Block per `HLE_FRAME_LOOP2=…` (+`HLE_FRAME_ARG2`)
   takten, bis `[0x40B4CC] != 0` (per `HLE_DUMP_RANGE=40B4CC,40B4D0` belegen).
3. Dann Submit-Nachweis: `ARM_MEMWATCHRANGE=10002800,0x200` (erster
   `SubmitCmdList`-Write), `HLE_GSPQ_PUMP=1` (Parser existiert:
   `gsp_process_cmdqueue`, Azahar-Layout, Log `[gspq]`), `HLE_LOG=1`
   (gsp-Kommandos 0x000C/0x0005).
4. Screenshot: `out\port.exe out\romfs --shot out\shot.bmp` mit
   `PORT_SHOT_FRAMES` ≥ 3000 (≈50 s) und ohne `HLE_GPU_SUBMIT`; Byte-Vergleich
   gegen den Platzhalter, danach optisch gegen `reports/azahar_topscreen.png`.
5. Falls `0x40B4CC` einfach nicht erreichbar ist: Wert notfalls per HLE direkt
   setzen (`[0x40B4CC] = 0x00101750`) und prüfen, ob der Dispatcher dann
   `0x00101750` pro Frame aufruft und die Kette bis `0x000C`/`0x0005` läuft.

WICHTIGE FALLEN
- `tools/decomp.py` überschreibt `code_decomp.bin` (alle Patches weg) →
  danach `apply_all_patches.py` + `patch_real_loader.py` + `patch_frame_callback.py`.
- `arm_trace_block` existiert **nur an Blockanfängen**; Watchdog-PCs wie
  `0x283488` sind KEINE Blockstarts (Sonden dort feuern nie).
- `getenv` pro Block ist teuer: Envs in `static` cachen; Läufe brauchen
  Geduld (Mount ≈ 120–180 s; `HLE_SCENE_READY` bremst massiv).
- `out/romfs_image.bin` ist **pristine**; keine Ketten-Patches mehr anwenden
  (früher Zyklen → Endlosschleife 0x212D70).
- `HLE_GPU_SUBMIT` ist ein **synthetischer** Test (beweist nur den Rasterizer,
  nicht das Spiel).
- `0x2834A8` ist der Allokator-Restplatzrechner, keine Queue; Allokationen sind
  gesund (37 Allocs, keine 0-Größe, Arena 0x62AA3C/0x800000 belegt).
- Service/Runtime-Schalter (Auswahl): `HLE_FILE_PRELOAD`, `HLE_BIGARENA`,
  `HLE_OBJ_FIX`, `HLE_SPAN_FIX`, `HLE_REG_FIX`, `HLE_DRAW_FIX`,
  `HLE_COUNT_FIX`, `HLE_GSP_IRQ`, `HLE_GSPQ_PUMP`, `HLE_FRAME_LOOP(+ARG)`,
  `HLE_FRAME_LOOP2(+ARG2)`, `HLE_CALL_LOG`, `HLE_SCAN_VALS`, `HLE_DUMP_RANGE`,
  `ARM_MEMWATCHRANGE`, `ARM_MEMWATCHRD`, `HLE_ALLOC_LOG`, `HLE_LOG`,
  `HLE_THR_LOG`, `PORT_SHOT_FRAMES`.

DOKUMENTATION IM REPO
- `docs/PORT_STATE.md` — vollständige Historie der Messungen, Patches,
  Schalter und offenen Punkte (dort zuerst lesen!).
- `docs/ROMFS_LOOKUP.md` — RomFS-Layout, IVFC-Strip, Ketten-Fix.
- `reports/azahar_topscreen.png`, `reports/azahar_trace.txt`,
  `reports/romfs_listing.tsv` — Referenzen aus Azahar.
