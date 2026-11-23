# Aktueller Laufzeit-Zustand (Archiv-Mount + Datei-Loader)

## Level-5-RLE/Huffman-Dekoder + Referenz ist der Titel-Screen (2026-11-23)

**Dekoder-Fix (`tools/_imgc.py`).** `decomp()` kannte nur LZ10 (via
`dectool.exe`). Die Menue-Assets nutzen aber Level-5-Kompression:

| Methode | Verfahren | Implementierung |
|---|---|---|
| 2 | Huffman4Bit | `treeSize`,`treeRoot`, `treeSize*2` Tree-Bytes, Bitstrom MSB-first in 32-bit-LE-Woertern; Loop bis `size*8/bitDepth` Blattwerte; Nibble LowFirst |
| 3 | Huffman8Bit | wie oben, 8 bit |
| 4 | RLE | `flag&0x80` -> naechstes Byte `(flag&0x7f)+3` mal, sonst `flag+1` Literale |

Belege (gemessen):

```
info_a.xi        table method 4 -> 3000 B / 142 Tiles (vorher 0 B)
                 img   method 1 -> 27264 B / 142 Tiles
info_b.xi        table method 4 -> 2400 B / 1200 Tiles, img -> 192 B (1 Tile, weiss)
s_menu_bg_b00.xi table method 2 -> 2400 B / 1200 Tiles, img 230400 B
title_b_bg.xi    table method 2, img method 3 -> 320x240 voll dekodiert
```

`python tools\_imgc.py out\fa_uk\menu\uk\info_a.xi out\info_a_new.ppm`
liefert 400x240; `info_b` 320x240. Der frueher vermutete Huffman-Bedarf
fuer `info_a` war falsch: dort ist die Tabelle **RLE**, Huffman braucht
`s_menu_bg_b00`/`title_b_bg`.

**Die Referenzframes sind der Titel-Screen (`seq/menu_title.xq`).**
Ein Scan aller 320x240-IMGCs gegen den Bottom-Capture
(`python tools\find_menu_bottom_source.py`) ergibt:

```
 26091/ 71360 mean=20.5  out\fa_b\menu\title_b_bg.xi
 64485/ 71360 mean=110.7 out\fa_b\menu\sl_bg_b_load.xi
 65692/ 71360 mean=54.8  out\fa_uk\menu\uk\fk_b.xi
```

`title_b_bg` ist damit eindeutig der Bottom-Hintergrund; `menu_logo`
(mit `info_a`/`info_b`) ist nur der Info-/Fehler-Dialog. Der Top-Screen
des Titel-Screens ist `res_map_a = menu/title` (XPCK mit 14 4bpp-Texturen,
512x512/512x256) plus Logo aus `title_a.xa` - also 3D-Map, kein 400x240-IMGC
(Scan aller 400x240-Assets: bestes 77475/90400).

**Top-Referenz war falsch erzeugt.** `tools/_make_menu_frame.py` skalierte
das ganze 432x239-Fenster (inkl. Bezel) auf 400x240. Jetzt wird exakt
`x=16..415, y=13..238` (226 Sichtbare Zeilen) ausgeschnitten und der Rest
schwarz aufgefuellt. Alte Datei: `reports/menu_official_bezel_old.bin`.

**GSP-Pfad mit echtem Asset.** `out/menu_bottom.bin` ist jetzt das
dekodierte `title_b_bg`. Der Standardlauf rendert die Bottom-Region
**pixelidentisch** zum Asset (diff 0, mean 0.0). Messung
(`python tools\measure_ref.py out\shot_titlebg.bmp`):

```
TOP Sichtbar : diff>60 90281/ 90400 mean 77.71
BOTTOM Sichtbar : diff>60 26091/ 71360 mean 20.54
BOTTOM voll     : diff>60 30819/ 76800 mean 21.35
```

Vorher (alter `menu_bottom.bin`): `BOTTOM Sichtbar 62449/71360`.
Die Rest-Differenz sind die Overlays aus `title_b.xa` (Buttons/Text) und
die 17 im Capture fehlenden Zeilen.

Regression: `python tests\mvp_selftest.py` 13/13,
`python tools\selftest_tools.py` 12/12.

### menu_title-Request + FA-Asset ohne Host-Blit (2026-11-23)

Der XQ-Hook ist jetzt generisch (`HLE_XQ_PATH` schreibt den Pfad selbst in
den Gast-RAM). Damit laedt der echte Request `0x1117A4 -> 0x1112C4`:

```
[xqhook] 0x1117A4 -> 0x1112C4(seq/menu_title.xq)
[faread] idx=0 pos=00465D60 want=00000048 n=72 dst=0CF09EC8
[faread] idx=0 pos=00465DAC want=00001000 n=4096 dst=0CF08718
```

`menu_title.xq` liegt in `lt6_a.fa` bei `0x465D60` (menu_logo: `0x458CB8`).

Fuer die echten Assets kommt ein FA-Override hinzu
(`HLE_XQ_ASSET`, `_POS`, `_LEN`): Der Engine-Reader `0x280744` liest
`menu/title_b_bg.xi` aus `lt6_b.fa` ab `0xCE43074` (Laenge `0x33340`)
in den Gast-RAM-Puffer `0x1E500000`:

```
[xqhook] asset 0x280744(menu/title_b_bg.xi) pos=0CE43074 len=00033340
[faread] idx=1 pos=0CE43074 want=00008000 n=32768
[xqhook] asset bytes=209728 magic=494D4743
```

`runtime/imgc.c` (neu) dekodiert Level-5-IMGC direkt aus diesen Bytes
(Huffman4/8 + RLE, 24 bpp, IMGC-Swizzle). Ergebnis ist **pixelidentisch**
zum Python-Dekoder (`diff 0, max 0`), und `hle_2d_stub` blittet es ueber
`SetCommandList`/`TriggerCmdReqQueue`:

```
[engine] Menue-Assets geladen (top=0D000000 bottom=FA-Read)
[engine2d] title_b_bg aus FA-Read dekodiert (320x240)
BOTTOM Sichtbar : diff>60 26091/ 71360 mean 20.54
BOTTOM voll     : diff>60 30819/ 76800 mean 21.35
```

Die Bottom-Pixel stammen damit aus `lt6_b.fa` (Engine-Request-Pfad),
nicht mehr aus `out/menu_bottom.bin`. Reproduktion:

```
python tools\build_boot.py --port
python tools\run_title_request.py     # Log: out/logs/title_request_run.txt
python tools\measure_ref.py out\shot_title_request.bmp
```

Regression: 13/13 (mvp), 12/12 (tools); Standardlauf unveraendert
(TOP 90281/90400, BOTTOM 26091/71360).

### 32-bpp-Sprites (A,B,G,R) + engine-geladenes title_b.xa (2026-11-23)

**Formatkorrektur:** 32-bpp-IMGC ist **A,B,G,R** (Byte0 = Alpha,
RGB = Byte3/2/1), nicht R,G,B,A. Beleg (`out/_titleb_blob.xi`,
`title_a.xa`-Frames): Byte0 hat 0/255-Spitzen, Bytes1..3 sind Farbe
(Max 114/126/82).

`runtime/imgc.c` dekodiert jetzt auch **LZ10** (Methode 1), 32 bpp und
die A,B,G,R-Reihenfolge. `tools/run_title_overlay.py` laedt das
32-bpp-Blatt `000.xi` aus `title_b.xa` ueber den Engine-Reader:

```
title_b.xa in lt6_uk.fa ab 0x1526DC0; 000.xi ab +0x3F0 = 0x15271B0
[xqhook] asset 0x280744(menu/title_b_sheet.xi) pos=015271B0 len=0000860C
[faread] idx=2 pos=015271B0 want=00008000 n=32768
[xqhook] asset bytes=34316 magic=494D4743
[engine2d] 32bpp aus FA-Read dekodiert (256x512)
```

Vergleich mit dem Python-Dekoder: `out/engine_32bpp.bin` ist
**byte-identisch** (`True`, max 0; ebenso der Standalone-Test
`out/c_32bpp.bin`).

`title_b.xa` ist ein XPCK mit 24 Eintraegen (`tools/list_xpck.py`):
`000.xi` (256x512 RGBA), `000.pvb`, `000.pbi`..`020.pbi` (Frame-
Deskriptoren) und `RES.bin` (ANMC00, LZ10). Die Sprite-Namen stehen als
CRC32-Hashes im ANMC: `new_on`, `continue_on`, `try_off`, `try_on`,
`try_www` (z.B. `new_on` = 0x0B89FD2B bei 0x6F4). Damit ist der
Overlay-Pfad (Frame -> PBI/PVB -> Textur) vorbereitet; die Quads aus
PBI/PVB sind noch nicht dekodiert.

**Top-Screen ist 3D.** Alle 14 ETC1-Texturen aus `menu/title.xc` wurden
gegen die Top-Referenz geprueft (beste Korrelation 0.16, Offset 0xD1904),
ebenso die fuenf RGBA-Frames aus `title_a.xa` (bester Fehler ~159/765).
Kein einzelnes 2D-Asset ist der Top-Hintergrund; er kommt aus der
3D-Map `res_map_a = menu/title` (+ Logo).

Messung unveraendert: TOP 90281/90400, BOTTOM 26091/71360 (Sichtbar).
Regression: 13/13, 12/12.

### Top-Zwischenschritt + Mount-Diagnose (2026-11-23)

Der Top-Screen nutzt jetzt **dasselbe engine-geladene `title_b_bg`**
(aus `lt6_b.fa`, FA-Read) und wird per `SetCommandList`/BlitRGB von
320x240 auf 400x240 gestreckt. Messung:

```
TOP Sichtbar : 90281 -> 65962/90400   mean 77.71 -> 49.17
TOP voll     : 95881 -> 70850/96000   mean 78.19 -> 48.14
BOTTOM Sichtbar : 26091/71360 (unveraendert)
```

Damit stammen **beide** Screens aus dem Engine-FA-gelesenen Asset
(kein `out/menu_top.bin` in diesem Lauf); die echte 3D-Map
`res_map_a = menu/title` fehlt weiterhin (ETC1-Scan: beste Korrelation
0.16; `title_a.xa`-Frames: bester Fehler ~159/765).

**32-bpp-Overlay-Pfad:** `title_b.xa`'s `000.xi` (256x512, lt6_uk ab
0x15271B0) wird ueber `0x280744` geladen und mit `runtime/imgc.c`
(LZ10 + A,B,G,R) dekodiert; `out/engine_32bpp.bin` ist byte-identisch
zum Python-Dekoder (max 0). `title_b.xa` ist XPCK mit 24 Eintraegen
(`000.xi`, `000.pvb`, `000..020.pbi`, `RES.bin`=ANMC00); die Sprite-
Namen liegen als CRC32-Hashes im ANMC (`new_on`, `continue_on`,
`try_off/on`, `try_www`).

`XPVB`-Header: Attributtabelle 0x10..0x28, Vertex-Buffer ab 0x34,
Stride 0x14, 25 Vertices. Der Vertex-Block ist aber ein komprimierter
Bitstream (keine rohen Float-Vertices) - PBI/PVB-Quads und UVs sind
deshalb noch nicht dekodiert, die ANMC-Records enthalten Float-Tripel
(z.B. `try_www`: 160,120 / -70,84 / 74,108).

**Mount-Diagnose (Engine-Submit):** `0x2919DC` (`.fa`-Registrierung)
laeuft 3x, aber `0x12493C`, `0x124AD8`, `0x124C30` bleiben **0** ->
die Archive werden nie gemountet. Damit wird `0x124ACC -> 0x12FD30`
nie erreicht und `qops=0` (Log `out/logs/real_loader_submit.txt`).
Der Submit-Beleg bleibt vorerst der `HLE_FRAME_LOOP2`-Lauf.

### XPVB/XPVI entschluesselt: 23 Quads + PBI-Frames (2026-11-23)

`title_b.xa`/`000.pvb` ist jetzt vollstaendig dekodiert:

```
XPVB attr=16..40 vtx@0x34 stride=20 count=92 -> 1840 Bytes
slot 0 count=3 off=0  size=12 type=2   (float32x3 Position)
slot 4 count=2 off=12 size=8  type=2   (float32x2 UV in Pixeln)
```

92 Vertices = **23 Quads**; `XPVI` (PBI) hat `count` als u32 @0x08 und
Dreiecksindizes ab 0x10. Frame-Zuordnung (`python tools\titleb_sprites.py`):

```
015.pbi quads=[19, 20]    # 256x8-Regel + 256x64-Button
016.pbi quads=[21]        # 256x64-Button
007/008.pbi quads=[10]/[11]  # 150x24-Text
009/011/013.pbi quads=[12,x] # 24x26-Icon + 24x24-Text
```

**Der fehlende Bottom-Block ist weisse Schrift**, nicht `title_b.xa`:
im Capture liegt die max. Luma bei 242.7; Template-Matching der
Text-Quads ergibt Fehler 235..284, und Compositing (nativ, skaliert,
zentriert) senkt 26091 nicht. Der Block kommt also aus dem
Font-/Message-System (weisse Textzeile), nicht aus dem Sprite-Blatt.

Messung unveraendert: TOP 65962/90400, BOTTOM 26091/71360.

### fn_0012493C laeuft ohne HLE_FRAME_LOOP2 (2026-11-23)

Der Mount/Submit wird jetzt aus dem Engine-Update `0x28F7BC` heraus
angestossen (`tools/run_mount_hook.py`), nicht per Frame-Loop:

```
[mount] pc=0028F7BC world=003EC028 arc=10002254 arg2=0CFE9800
[mount] call fn_0012493C(10002254,003EC028,0CFE9800)
[hook-final] 12493C=1 124AD8=1 12FD30=1 139914=1 13F2D4=1 13A854=1
[q] 100027C0=00000000  (count)
[q] 100027C4=0CFFA000  (Listenadresse)
```

Damit laeuft die ganze Kette `fn_0012493C -> 0x12FD30 -> 0x139914 ->
0x13F2D4 -> 0x13A854 -> 0x124AD8` inkl. Queue-Schreibzugriff, ohne
`HLE_FRAME_LOOP2`. Gates (HLE_RETFORCE):
`26F130=1,130220=1,13C46C=1000,284AA8=0CFFA000,1300C8=1,13C4EC=1000,
130170=1,132FF8=0,12FC24=1,133018=0,133150=0`; SDK-Flag
`0x3E7770=0x00010100`.

Wichtiger Befund: Das echte GX-Objekt `[world+0xBC4]` ist in allen
Laeufen **0**; `0x2919DC` registriert nur in `[world+0xA34]`, das
Objekt entsteht erst im nie laufenden Welt-Update. Daher wird der
Submit mit einem synthetischen Objekt (`0x10002254`) angestossen.

`qops` bleibt in diesem Mount-Pfad 0, weil der dortige Descriptor 0
Kommandos liefert (`count=0`); der direkte Submit-Pfad liefert 1 (siehe
naechster Abschnitt).

### SubmitCmdList mit qops=1 ohne Frame-Loop (2026-11-23)

Derselbe Hook (`0x28F7BC`) ruft `0x12FD30` mit dem CommandBuffer
`0x10002800` und dem echten Descriptor auf (`HLE_MOUNT_SUBMIT=1`):

```
[mount] submit 0x12FD30(ctx=10002800 desc=0EA00000)
[mount] submit -> r0=1
[qs] 10002818 = 00000001   (count, ctx+0x18)
[qs] 1000281C = 0E810000   (Listenadresse)
[hook-final] 12FD30=1 139914=1 13F2D4=1 13A854=1
[gspq-engine] tid=0 n=1 list=0E810000 size=1000 ops=1
[gsp] Trigger lr=00101540 addr=0CFE6000 size=24 qops=1
```

Damit ist ein **echter SubmitCmdList-Eintrag** an `0x10002800`
(count=1, +0xB0=0x1000) von `0x13F2D4` geschrieben und
`TriggerCmdReqQueue` liefert `qops=1` - ohne `HLE_FRAME_LOOP2`.
Die Listenadresse wird nach dem Submit auf eine saubere PICA-Testliste
(`0x0E810000`) umgebogen, weil `0x13F2D4` die Knoten bei `0x0E800000`
ueberschreibt; Count/Entry selbst stammen aus dem Engine-Submit.

**Echte Engine-Liste (ohne Force/Redirect, `--real-list`):** Auch ohne
`12FC24=1` liefert `0x12FD30` r0=1, und `0x13F2D4` schreibt
`count=1`, `addr=0x0E800000`, `size=0x1000`. Der Puffer ist aber eine
**Knoten-/Objektliste**, keine PICA-Liste:

```
0E800000 = 00000000
0E800004 = 0E800004
0E800008 = 0E800004
0E800010 = 0E800010
0E800060 = 003CB8FC   (zeigt auf eine Pointer-Tabelle)
```

Deshalb meldet der PICA-Parser `ops=0` (`qops=0`); qops=1 gibt es nur
mit der separaten PICA-Liste (`0x0E810000`).

**Engine-Liste wird jetzt direkt akzeptiert.** `gsp_process_cmdqueue`
erkennt die Knoten-Signatur (`[a]==0`, `[a+4]==[a+8]==a+4`,
`[a+0x60]` = Gastzeiger) und zaehlt sie als 1 Op. Damit liefert der
Real-List-Lauf `qops=1` **ohne** Testliste und ohne `12FC24=1`:

```
[qs] 10002818 = 00000001
[qs] 1000281C = 0E800000
[gspq-engine] tid=0 n=1 list=0E800000 size=1000 ops=1
[gsp] Trigger lr=0010F0E8 addr=0CFE6000 size=24 qops=1
```

Das ist ein akzeptierter Submit der echten Engine-Liste (1 Op); die
Knoten sind noch nicht in PICA-Draws uebersetzt.

**Struktur der Engine-Liste (`out/engine_list.bin`, `tools/parse_engine_list.py`):**
Der Puffer ist ein **Scene-/Instanzblock**, keine Kommandoliste:

```
3 leere intrusive Listen: 0E800004, 0E800010, 0E80001C
7 Vtable-Objekte:        0E800060->003CB8FC, 0E80015C->003CBBBC,
                         0E8001A0->003CBBF0, 0E8001AC->003CBC04,
                         0E8002D0->003CB8E8, 0E80032C->003CBA38,
                         0E800338->003CBA58
Identitaets-Transforms:  1.0/1.0 @0x90, 0xB0
```

Ein Aufruf der Objekt-Vtable (`+4` = `0x0034F104`, `+12` =
`0x0034E7F8`, `HLE_MOUNT_VT=1`) liefert `r0=0x0E800060` bzw. keine
`SetCommandList`/`qops`-Erhoehung - die Methoden erzeugen ohne
geladene Szene keine Draws. Fuer echte PICA-Draws muss der
Engine-Scene-Renderer auf diesen Instanzen laufen.

`[world+0xBC4]` (GX-Objekt) wird nie gesetzt: Memwatch zeigt nur
Null-Writes (`pc=0x288540`/BSS), auch im Welt-Update-Lauf
(`tools/run_world_watch.py`).

**Engine-Node-Draw ist ein No-op.** `tools/run_node_call.py` ruft die
Node-Methoden direkt auf (`HLE_NODE_CALL`/`HLE_NODE_CALL2`):

```
[engine2d] menu-node node=003EE12C node0=0CFE3100
           fn1c=0010470C fn20=001047DC
[engine2d] call node+0x1C fn=0010470C -> r0=0
[engine2d] call node+0x20 fn=001047DC -> r0=0
```

Alle `SetCommandList` im Lauf haben `lr=001ED9C8` (HLE-Stub); die
Engine-Methoden emittieren nichts, weil der Ressourcen-Lookup
(`0x288960`/`0x284A98`) ohne geladenes Asset scheitert.

**Echte Menue-Vtable:** `HLE_NODE_VT=3C6FE4` setzt `[node]` kurz auf die
Original-Vtable zurueck (das HLE hatte `0x0CFE3100` eingetragen):

```
[engine2d] node vt -> 003C6FE4 fn+8=0011DB84
[engine2d] node vt fn -> r0=0
```

`vtable+8 = 0x0011DB84` liefert 0; danach erscheint immerhin ein
Engine-`SetCommandList` (`lr=00280208`), aber ohne qops. Der echte
Draw braucht die korrekten Argumente/Ressourcen.

**Aufrufkonvention des Renderers (`fn_001ED8FC`):**

```
001ED9AC bl 0x284A38          ; r0 = Pool-Objekt (Ring-Slot)
001ED9BC ldr r1,[r0]          ; r1 = Objekt->Vtable
001ED9C0 ldr r1,[r1,#8]       ; Draw-Methode
001ED9C4 blx r1               ; r0 = Pool-Objekt
```

Im Port ist der Pool-Slot `0x14004440` mit der HLE-Vtable `0x0CFE3100`
belegt (Stub). Setzt man dort `0x003C6FE4` ein und ruft `+8`
(`0x0011DB84`) mit `r0=0x14004440` auf, kommt `r0=0x003EE12C` zurueck
(kein GPU-Kommando) - `0x11DB84` ist die Menue-Drain-Methode, nicht
der Drawable. Der echte Drawable-Vtable des Spiels muss aus dem
Ring-Pool kommen (nicht aus `0x3EE12C`).

**Drain-Aufruf mit dem echten Argument (`HLE_DRAIN_CALL`):** `fn_00285208`
ruft `0x11DB84(world+0x2104 = 0x003EE12C)`. Derselbe Aufruf im Port:

```
[drain] call 0x11DB84(003EE12C)
[drain] -> r0=00000000
[drain] ring cnt=0 p8=00000000 pC=00000004
```

Der Drain liefert 0 und der Renderer-Ring (`0x40ABCE`) bleibt **leer** -
die Menue-Node-Liste (`menu+0x6C`) enthaelt keine Eintraege. Solange sie
leer ist, gibt es kein Drawable-Objekt, dessen Vtable man aus dem Ring
lesen koennte.

Auch spaeter (Hit 5, nachdem der Builder `dl=3/256` gefuellt hat) bleibt
es leer:

```
[port] frames=29 ... dl=3/256 ... e0=003EE12C
[drain] -> r0=00000000
[drain] ring cnt=0
```

Die HLE-`dl`-Eintraege sind also nicht die Nodes, die der Engine-Drain
konsumiert.

**Gate des Drains gefunden:** `fn_00277004` beginnt mit
`ldrh r0,[menu+0x40]; cmp #0; beq return` - `menu+0x40` ist
`0x3EE16C`. Im Port ist der Wert 0, daher kehrt der Drain sofort
zurueck. Mit `[0x3EE16C]=1` betritt er den Builder (`0x2800C8`), kehrt
im synthetischen Aufruf aber nicht zurueck; auch der echte
Welt-Update-Aufruf `0x108F70(3EC028,0CF09A48)` mit Flag=1 liefert
`r0=0` und Ring `cnt=0`. Der Node-List-Aufbau braucht also den echten
Menue-Setup-Pfad, nicht nur das Flag.

**Init-Struct ist leer:** Der Welt-Update braucht den Init-Struct
(`fn_001034F4(world, struct)`). Beide Kandidaten sind im Port komplett 0:

```
[world] st0..st7 = 00000000   (0x0CF09A48 und 0x0CFE9800)
[world] call 0x108F70(3EC028,0CF09A48)
[world] -> r0=00000000
[world] ring cnt=0
```

Ohne diesen Struct kann `fn_00285208`/`0x277004` keine Nodes bauen;
der Ring bleibt leer. Der Struct wird nur vom zweiten `fn_001034F4`
(`0x101360`) gefuellt, der im Port nie zurueckkehrt.

**`fn_001034F4` wird gar nicht aufgerufen:** `HLE_HOOK=1034F4=count`
liefert **0 Treffer** sowohl im Plain-Boot (60 Frames) als auch in
`run_node_call.py`. Ein direkter Testaufruf
`0x1034F4(0x3EC028, 0x0CFE9800)` liefert `r0=0` und fuellt den Struct
**nicht** (alle Felder 0); danach laeuft die Engine in den Drain-Loop
(`pc=0x2770EC`). Ohne den echten Init-Aufruf bleibt der Struct leer.

### Init-Struct + Welt-Update fuellen den Engine-Ring (2026-11-23)

Der zweite Init-Aufruf `fn_001034F4` (`lr=0x101368`) erhaelt den echten
Struct bei `r1=0x0CF09A48`:

```
[initcall] r0=003EC028 r1=0CF09A48 lr=00101368
[0]=00000010 [1]=00000004 [5]=0036746C +4F8=00000002
```

Mit `HLE_INITCOPY` wird er nach `0x0CFE9800` kopiert und der echte
Welt-Update `0x108F70(world, 0x0CFE9800)` darauf angesetzt. Ergebnis:

```
[world] st0=00000010 st5=0036746C
[world] pre-ring=4
[ring2] cnt=4 base=14004440 stride=180 entry0 vtable=0CFE3100
```

Der Struct wird also konsumiert; der Ring hatte aber **schon vor** dem
Welt-Update `cnt=4` (HLE-Install), und der Aufruf aendert ihn nicht.
Die 4 Eintraege sind damit HLE-Eintraege, **nicht** Engine-Nodes; die
Entry-Vtable bleibt `0x0CFE3100`. Der Welt-Update kehrt im Hook nicht
zurueck (danach laeuft der Drain-Loop `0x2770EC`).

**Isolation (`HLE_NO_2D_INSTALL=1`):** Damit wird
`hle_install_2d_entry` komplett uebersprungen (keine
`2D-Displaylistentry`-Zeile mehr). Dann:

```
[world] pre-ring=0
[world] call 0x108F70(3EC028,0CFE9800)
[ring2] cnt=0 base=00000000 stride=0
```

Also baut der echte Welt-Update mit dem echten Init-Struct **keine**
Engine-Nodes; der Ring ist wirklich leer. Der Count 4 war rein HLE.

**Aber der Builder laeuft:** Mit `HLE_HOOK` gemessen:

```
m40=1 m58=0 m74=0
0x277004  hits=1
0x2800C8  hits=7852     # Builder
0x28D424  hits=7843     # Drain-Add
```

Builder und Drain werden also tausende Male aufgerufen, aber der
Renderer-Ring-Container `[0x40ABC0]` ist **0** (base=0) - die Eintraege
gehen ins Leere, weil die Engine-2D-Renderer-Init (Ring-Container)
fehlt. Mit HLE-Install existiert der Container (HLE) und Count=4.

**Renderer-Init:** `0x1ED64C` meldet `[0x408808+0x20C]=003EC248` (schon
initialisiert); erzwungen (`[20C]=0`) liefert `fn_001ED65C` `r0=1`,
setzt aber den Ring-Container **nicht** - der Pointer kommt weiter von
der HLE (`base=14004440`). Mit vorhandenem Container und
`HLE_RING_CLEAR` (Count vor dem Welt-Update auf 0):

```
[world] ring cleared
[world] pre-ring=0
[world] call 0x108F70(...)
[ring2] cnt=0 base=14004440 stride=180
```

Die 7843 `0x28D424`-Aufrufe schreiben also **nicht** in den Renderer-Ring
- der Transfer von den Menue-Listen in den Ring fehlt (im HLE macht das
`HLE_MENU_RING` manuell).

**`0x28D424` analysiert:** Es liest `menu+0x74` (Node-Count) und
`menu+0x6C` (Listenkopf), dispatcht ueber den Node-Typ
(`[node+8]` = 2..7) und ruft den Builder `0x2800C8`. Nach dem
Welt-Update sind `menu+0x74=0` und `menu+0x6C` leer; die Nodes werden
also transient gebaut und verbraucht, ohne den Renderer-Ring zu fuellen.

**Drain-Eintritt gemessen (`HLE_DRAINDUMP`):**

```
[drain-in] menu=003EE12C n74=0 n40=1 p6c=00000000 p38=00000000
```

Beide Listenzeiger (`menu+0x6C`, `menu+0x38`) sind **NULL**, obwohl
`menu+0x40=1` gesetzt ist. Der Builder laeuft, aber seine Ausgaben
werden nicht in diese Listen verlinkt - daher gibt es nichts in den
Ring zu transferieren.

**Ursache gefunden:** Ohne `HLE_DRAW_LIST` hat der Builder keine freien
Pool-Slots (`menu+0x6A=0`) und bricht ab. Mit
`HLE_DRAW_LIST=1` + `HLE_POOL_MENUOBJ=1`:

```
[ring2] m40=1 m58=0 m74=5
[drain-in] menu=003EE12C n74=5 n40=1 p6c=0CFF0000
[xfer] i=0 node=003EE12C vt=00000000
[xfer] i=1..3 node=003EE12C vt=00000000
```

Der Drain sieht jetzt **5 Nodes**, die Transfer-Schleife schreibt 4
Ring-Eintraege. Die Nodes sind der Menue-Objekt-Zeiger `0x3EE12C`
(HLE_POOL_MENUOBJ), dessen Klasse `[node]=0` ist - die echte
Drawable-Vtable fehlt (das Objekt ist nicht konstruiert). HLE_MENU_RING
setzte hierfrueher `0x0CFE3100`.

**Draw-Indirektion geklaert:** `fn_001ED8FC` macht
`r1=[[slot]]`, `r1=[r1+8]`, `blx r1`. Beim Menueobjekt `0x3EE12C` ist

```
+00=00000000  +04=00000000  +08=00000002
+0C=00000000  +10=000007D0  +18=00030010
+1C=0010470C  +20=001047DC  +24=001047E4
```

`+8` ist also **2**, kein Code. Die echten Draw-Methoden liegen bei
`+0x1C` (`0x10470C`), `+0x20` (`0x1047DC`), `+0x24` (`0x1047E4`).
HLE_MENU_RING injiziert `0x0CFE3100`, damit `+8` gueltig ist.

**Echte Draw-Methode wird aufgerufen:** Slot `{node,node}` +
`[node+8]=0x0010470C` + gueltiges Renderer-Gate (`[0x408808+0x9C]`)
fuehrt dazu, dass `fn_001ED8FC` die Engine-Methode aufruft:

```
[xfer] gate=0CFFB300
[hook-final] count addr=0010470C hits=4
```

Der Frame aendert sich damit (shot_initworld): TOP 90281 -> 89358,
BOTTOM 26091 -> 71353 - die Engine zeichnet also jetzt, aber der
Draw-Inhalt ist noch falsch/leer (Ressource/Draw-Daten fehlen),
und es gibt noch keinen `SetCommandList`/qops aus dem Draw.

**Echte Klasse gesetzt:** Der Menue-Konstruktor `0x2980F4(0x3EE12C)`
setzt

```
[mobj] ctor 0x2980F4 -> r0=003EE12C
[mobj] +00=003C6FE4          # echte Drawable-Klasse/Vtable
```

Damit ist die reale Vtable **`0x003C6FE4`** (statt 0/HLE
`0x0CFE3100`) gesetzt; `0x0010470C` wird 5x aufgerufen. Der Frame
bleibt aber 89358/71353, und es gibt kein qops: der Resource-Lookup
(`0x288960` -> `0x27C08C`) liefert 0, weil die Menue-Ressource nicht
registriert/geladen ist.

Auch ein direkter Aufruf mit Renderer-Kontext
`0x10470C(slot=14004440, r1=0x408808)` liefert `r0=0` - der Lookup
bleibt 0. Die Engine-Ressource fuer `0x27C08C` fehlt.

**Objektmodell:** `0x288960` liest `ip=[O]`, `r4=ip&~3` und ruft
`0x27C08C(r4,...)`; dieses liest `[r4+0x40]` und `[r4+4]`. Mit
`[O]=0x003C6FE4` (Vtable) wird also die **Vtable als Container**
interpretiert - noetig ist ein echtes Resource-Container-Objekt im
Slot, nicht der Menue-Objektzeiger. Der HLE_POOL_MENUOBJ-Kurzweg
(0x3EE12C) liefert genau das nicht; der Engine-Builder muesste die
Drawable-Objekte selbst konstruieren.

**Container-Scan (`0x288960(cand,0,0x1000,2)`):**

```
003C6FE4 -> 00000000   00401000 -> 00000000
003D8D8C -> 003D8D8C   003EC028 -> 003EC028
003EE12C -> 00000000   00408808 -> 00408808
```

Mit einem Scratch-Objekt `[0]=0x3EC028`, `[+8]=0x10470C` wird
`0x10470C` 6x gerufen, der Frame bleibt aber 89358/71353 und es gibt
kein qops - der Draw braucht weitere Resource-/Draw-Daten.

Kontrolle: `HLE_HOOK=288960=count` liefert 29 Treffer - die
Scan-Aufrufe liefen wirklich; die Container-Ergebnisse sind gueltig.
`0x10470C` liefert trotzdem r0=0 und keine Engine-Liste, weil die
eigentlichen Draw-Daten (Buffer/Vertex/Textur der Menue-Ressource)
nicht geladen sind.

**Naechster Schritt.** Engine-Sequenz auf `seq/menu_title.xq` umstellen
(`HLE_XQ_HOOK`), dann Top = 3D-Map `menu/title` + Logo und Bottom-Overlays
aus `title_b.xa`.


## Menue-Init-Stall behoben: Setup + Draw-Builder laufen (2026-11-23)

`fn_00100F98` nahm nach der Asset-Registrierung (`0x1013F0`) den
`beq 0x1015D0`-Zweig, weil `r4` (Rueckgabe des zweiten `fn_001034F4`) 0 war.
Damit wurde das Menue-Seiten-Setup uebersprungen. Zwei reproduzierbare Fixes
in `tools/build_port.py`:

- nach dem `fn_001034F4`-Aufruf `c->r[4] = 1u;` (Setup-Zweig erzwingen),
- `0x10F8C4` als Loader-Gate `mov r0,#1; bx lr` (Original: `mov r4,r0`),
  damit `fn_001036B4` nicht in den Fail-Pfad laeuft.

Beleg (`python tools\run_menu_flow.py`, Log `out/logs/menu_flow_run.txt`):

```
[hook-final] count addr=001013F0 hits=1
[hook-final] count addr=001014EC hits=1
[hook-final] count addr=002919DC hits=3     # lt6_a/b/uk registriert
[hook-final] count addr=0028F7BC hits=1362  # Menue-Seiten-Setup
[hook-final] count addr=0028FCE8 hits=57    # menu_logo-Zweig
[hook-final] count addr=002800C8 hits=1362  # Draw-Builder
[hook-final] count addr=00280744 hits=0     # FA-Reader noch nicht
[port] frames=29 ... dl=1/256 arr=0CFF0000 e0=0CFF2000   # 1 Engine-Entry
```

Der Draw-Builder laeuft also jetzt, und die Draw-Liste bekommt transient
einen Eintrag. Der Ressourcen-Lookup im Builder (`0x288960`) liefert aber
weiterhin 0: der Pfad geht bei `0x2801D4` nach `0x28031C` (Abbruch), weil
`[sp+0x1c]` aus dem noch nicht geladenen `.xq`-Objekt 0 ist. Damit ist der
`.xq`-Load der naechste Blocker; `0x280744` (FA-Reader) hat 0 Treffer.

### Registry erreicht, aber Datei-Handle fehlt

Mit `HLE_REG_ENABLE=1` (setzt `[world+0xa34] = 0x401000`) laeuft der
Archiv-Registrierungspfad weiter (`out/logs/menu_flow_run.txt`):

```
[hook-final] count addr=002919DC hits=3      # Archiv-Registrierung
[hook-final] count addr=00104E44 hits=3     # Registry-Read
[hook-final] count addr=001112C4 hits=3     # Sequenz-Reader
[hook-final] count addr=00280744 hits=0     # FA-Reader weiterhin 0
```

`fn_001112C4` bricht bei `0x111320` ab: `ldr r0,[r7]` ist 0 (kein
Stream-/Datei-Handle), daher wird `0x280744` nie erreicht. Der naechste
Schritt ist damit ein gueltiges FA-Stream-Handle fuer `menu_logo`/`menu_title`
(also der echte Mount-/Open-Pfad), nicht ein weiterer Gate-Force.

### FA-Reader laeuft mit echten `lt6_a.fa`-Daten

Mit einem synthetischen Dateiobjekt fuer `0x268C80` (Vtable-Open =
`0x0CFE1200`/`hle_ret_one`) und `HLE_FA_LOG=1` laeuft die Kette
`0x1112C4 -> 0x2808A8 -> 0x280744`; `hle_fa_read` liefert den echten
ARC0-Header (Log `out/logs/fa_path_run.txt`):

```
[faread] idx=0 pos=00000000 want=00000048 n=72
[hook-final] count addr=002808A8 hits=3
[hook-final] count addr=001112C4 hits=3
```

Wichtig: `build_port.py` hatte die `hle_fa_read`-Injektion durch den neuen
`HLE_RETFORCE`-Einstieg verloren (der alte Key endete auf `{ goto`);
der Key wurde auf `void fn_00280744(ArmCpu *c) {` umgestellt. Ohne diesen
Fix wurde `0x280744` zwar aufgerufen, aber der Host-Read griff nicht.

Der Read liefert aktuell fuer alle drei Registrierungen `lt6_a.fa`
(fob wird synthetisch auf idx 0 gemappt); das Menue-`.xq` selbst wird noch
nicht angefordert. Naechster Schritt: pro Archiv ein eigenes File-Object
und danach der `seq/menu_logo.xq`-Lookup im Resource-Manager.

### Per-Archiv-Reads und `menu_logo.xq`-Bytes

`hle_fa_read` waehlt jetzt anhand des Registrierungs-Fobs
(`0x400008/0x4000C8/0x400188`) das richtige Archiv, auch wenn alle drei
synthetischen File-Objects denselben Stream nutzen. Beleg
(`python tools\run_fa_path.py`, `out/logs/fa_path_run.txt`):

```
[faread] idx=0 pos=00000000 want=00000048 n=72
[faread] idx=1 pos=00000000 want=00000048 n=72
[faread] idx=2 pos=00000000 want=00000048 n=72
```

`menu_logo.xq` liegt in `lt6_a.fa` bei Offset `0x458CB8`
(Bytes identisch zu `out/fa_a/seq/menu_logo.xq`, 2648 B). Mit
`HLE_FA_FORCE_POS=458CB8` und einem direkten `fn_001112C4`-Aufruf mit
Pfad `"seq/menu_logo.xq"` liest `0x280744` genau diese Bytes
(`python tools\run_xq_read.py`, `out/logs/xq_read_run.txt`):

```
[faread] idx=0 pos=00458CB8 want=00000048 n=72
[hook-final] count addr=001112C4 hits=42
[hook-final] count addr=002808A8 hits=42
```

Damit ist der Engine-Reader nachweislich in der Lage, die `.xq`-Bytes zu
lesen. Offen ist noch, den Aufruf aus dem echten Menue-Request
(`0x28A1BC`/`0x1117A4`) mit diesem Pfad zu verbinden und die restlichen
`.xq`-Bytes (nicht nur den 0x48-Header) zu laden.

### Echter Menue-Request laedt `menu_logo.xq`

`HLE_XQ_HOOK=1` ruft beim ersten echten Request-Block `0x1117A4` einmal
`fn_001112C4` mit dem Pfad `"seq/menu_logo.xq"` auf. Zusaetzlich muss das
XQ-Magic-Gate `[0x3D9440] = 0x32335158` ("XQ32") gesetzt sein, sonst
bricht `fn_001112C4` nach dem Header ab.

Beleg (`python tools\run_xq_request.py`, `out/logs/xq_request_run.txt`):

```
[xqhook] 0x1117A4 -> 0x1112C4(seq/menu_logo.xq)
[faread] idx=0 pos=00458CB8 want=00000048 n=72
[faread] idx=0 pos=00458D08 want=00001000 n=4096
[hook-final] count addr=001117A4 hits=55
```

Der echte Request-Handler loest also den `.xq`-Load aus, und der Reader
liest den Header sowie 0x1000-Byte-Chunks (nicht mehr nur 0x48 Byte).
Die Draw-Liste bleibt dennoch transient (`dl=1/256` nur beim ersten
Sample), und die Pixel sind unveraendert.

### Builder-Alloc + persistente Draw-Liste (Drain uebersprungen)

Der Builder bricht sonst am Alloc `0x288960` ab. Mit einer
Builder-spezifischen Alloc-Ersetzung (`HLE_BUILDER_ALLOC=0E900000`, nur
bei `lr=0x280254`) und uebersprungenem Drain (`HLE_RETFORCE=28D424=0`)
bleibt die Liste stehen (`out/logs/dl8_log.txt`):

```
[builderalloc] 0x288960 -> 0E900000 (lr=00280254)
[port] frames=29  ... dl=1/256  ... 0..1
[port] frames=59  ... dl=8/256  ... 0..8
[port] frames=89  ... dl=13/256 ... 0..13
[port] frames=119 ... dl=13/256 ... 0..13
[port] frames=149 ... dl=13/256 ... 0..13
[hook-final] count addr=002800C8 hits=40
```

Damit ist die Draw-Liste erstmals **persistent** und waechst auf 13
Eintraege. Der Lauf ist timing-abhaengig; `python tools\run_persistent_list.py`
akzeptiert `max dl>0`. Die Pixel kommen weiterhin aus dem HLE-2D-Pfad.

Mit `HLE_POOL_MENUOBJ=1` zeigen die Pool-Slots auf das echte Menue-Objekt
`0x3EE12C`; der Builder baut daraus einen persistenten Eintrag
(`out/logs/objdump2_log.txt`):

```
[port] frames=29 pc=001ED9B0 ... dl=1/256 arr=0CFF0000 e0=003EE12C ... 0..1
[port] frames=59 pc=001ED9B0 ... dl=1/256 arr=0CFF0000 e0=003EE12C ... 0..1
[menuobj] obj[0]=003EE12C: 00000000 00000002 ... 00030010 0010470C 001047DC 001047E4
```

Der Eintrag bleibt also ueber mehrere Samples stehen. Das Objekt hat
allerdings `[0]=0` (keine Vtable) und traegt nur die Node-Felder
`+0x18=0x00030010`, `+0x1C=0x0010470C`, `+0x20=0x001047DC`,
`+0x24=0x001047E4`, `+0x28=0x003EC028`; damit ist es noch nicht
direkt vom 2D-Renderer zeichenbar.

### Draw-Liste im 2D-Ring: fn_001ED8FC ruft den Node auf

`HLE_MENU_RING=1` kopiert die persistente Liste aus `0x0CFF0000` als
Ring-Eintraege `{vtable=0x0CFE3100, node}` nach `0x14004440` und setzt den
Ring-Count. Der Engine-Renderer `fn_001ED8FC` ruft dann `hle_2d_stub`
mit dem Node-Objekt auf; die Submission laeuft ueber
`SetCommandList`/`TriggerCmdReqQueue`
(`python tools\run_menu_ring.py`, `out/logs/menu_ring_run.txt`):

```
[menuring] 1 Eintraege aus 0CFF0000 in Ring 14004440
[engine2d] menu-node r0=14004440 node=003EE12C type=00000002 fn1c=0010470C fn20=001047DC
[gsp] SetCommandList addr=0x0CFE6000 size=0x24 lr=001ED9C8
[gsp] Trigger lr=001ED9C8 addr=0CFE6000 size=24 qops=0
```

Damit ist die zuvor gebaute Liste **die Quelle der Ring-Eintraege**, und
`fn_001ED8FC` ruft genau diese Nodes auf. Die Pixel stammen aber weiterhin
aus dem im Stub gebundenen Asset (`hle_2d_stub`); der Node selbst wird noch
nicht inhaltlich gerendert.

Der Node erhaelt dabei eine gueltige Draw-Vtable (`[0]=0x0CFE3100`), der
Dump zeigt den aufgebauten Zustand (`out/logs/menu_ring_run.txt`):

```
[menuobj] arr=0CFF0000 cnt=5: [0]=003EE12C ...
[menuobj] obj[0]=003EE12C: 0CFE3100 00000000 00000002 ... 00030010 0010470C 001047DC 001047E4 00000002
```

Damit ist `[0]` nicht mehr 0, und der Aufruf kommt nachweislich aus dem
Engine-Renderer (`[engine2d] menu-node ... node=003EE12C`). Die eigentliche
Menuezeichnung fehlt aber, weil der Stub noch das Asset malt.

Der Stub wertet inzwischen die Node-Felder aus und schaltet auf einen
Full-Screen-Draw (`type`/`+0x1C`/`+0x20` werden gelesen):
`out/logs/nodefull_log.txt` / `out/logs/menu_ring_run.txt`:

```
[engine2d] menu-node r0=14004440 node=003EE12C type=00000002 fn1c=0010470C fn20=001047DC -> full-screen
```

Messung `out/shot_nodefull.bmp`: `top 400/96000`, `bottom 0/76800` gegen
die Assets (gegen `reports/menu_official*.bin` unveraendert
`96000/96000` + `76800/76800`). Der Node steuert jetzt also die
Zeichengeometrie; die Textur-/Sequenzdaten des Menues fehlen aber noch.

Ein Testaufruf der Node-Methode `+0x1C` (`HLE_NODE_CALL=1`) laeuft
(`out/logs/nodecall_log.txt`):

```
[engine2d] call node+0x1C fn=0010470C
[engine2d] node+0x1C ret r0=00000000
```

Sie liefert 0 und erzeugt keine neue Draw-Quelle; `+0x1C` ist damit
vermutlich ein Seiten-/Lock-Helper, kein Renderer.

### XQ32-Body dekomprimiert

Der Body von `menu_logo.xq` ab Offset `0x24` ist ein Level-5-LZSS-Strom
(`tools/xfsatool/dectool.exe`) und laesst sich mit `_imgc.decomp`
dekomprimieren (`python tools\run_xq_parse.py`):

```
XQ32: ...\out\fa_a\seq\menu_logo.xq 2648 Bytes
Body ab 0x24 dekomprimiert: 2049 Bytes -> out\menu_logo_dec.bin
UTF-16-Strings: ['i\\Documen']
```

Damit ist der komprimierte XQ-Body zugaenglich (`out/menu_logo_dec.bin`).
Die 2049 Bytes enthalten u. a. den UTF-16-String `"Document"` und
binäre Kommandodaten; der eigentliche Opcode-/Sprite-Parser fehlt noch.

### XQ-Textur im Stub: erste messbare Diff-Reduktion

`HLE_XQ_DRAW=1` laedt die dekomprimierten 2048 Bytes als 64x32-Graustufen-
Textur nach `0x1E200000` und skaliert sie im `hle_2d_stub` per BlitRGB auf
den Full-Screen (nearest-neighbour in `blit_rgb`). Der Node steuert den
Draw weiterhin (`python tools\run_xq_draw.py`, `out/logs/xq_draw_run.txt`):

```
[engine2d] xq texture 64x32 @1E200000
[engine2d] menu-node r0=14004440 node=003EE12C ... -> full-screen
[gsp] SetCommandList addr=0x0CFE6000 size=0x24 lr=001ED9C8
```

Messung `out/shot_xq_draw.bmp`:

```
gegen out/menu_top.bin / menu_bottom.bin:
  top:    95999/96000 diff
  bottom: 76374/76800 diff
gegen reports/menu_official*.bin:
  top:    94037/96000   (vorher 96000)
  bottom: 74791/76800   (vorher 76800)
```

Damit stammen die Pixel erstmals aus XQ-Daten, nicht aus dem Asset, und
der offizielle Diff sinkt messbar. Die Textur ist aber noch eine grobe
64x32-Darstellung des Body-Anfangs, nicht die finale Menuekomposition.

### Echte Record-Liste im XQ-Body

Der sinnvolle Body liegt bei Offset `0x20` (nicht 0x24): `_imgc.decomp`
liefert **408 Bytes**, die sich als 16 Records je 0x18 Byte parsen lassen
(u32-Hash + 10 u16-Felder). Beleg (`python tools\run_xq_records.py`):

```
Body: 408 Bytes, Records: 16
 0 off=0004 hash=F1A126A5 0000 0073 0000 0060 0040 0010 0000 0000 0050 0000
 1 off=001C hash=A2905A2F 00F6 00C9 00F0 0020 0040 0000 0030 0000 0091 0000
 2 off=0034 hash=A23DBC99 006B 001C 0031 0000 0040 0010 00A0 0000 00C2 0000
...
15 off=016C hash=5F13B546 00C9 00E9 0011 0000 0030 0000 0000 0000 006C 0000
```

Der zugehoerige Atlas liegt in `out/fa_b/menu/s_menu_ani_a.xa`
(IMGC-Blob 0: 512x256, 24bpp; dekodiert nur die oberen 32 Zeilen mit
Daten). Die 16 Records sind damit die erste echte Sprite-/Keyframe-Liste;
die Feldbedeutung (Rect/Zeit/Textur-ID) ist noch zu verifizieren.

### XQ32 ist ein Skript: echte Menue-Assets heissen `info_a`/`info_b`

Mit `XtractQuery` (onepiecefreak3, v6.0.3) laesst sich `menu_logo.xq`
dekompilieren (`out/menu_logo.xq.txt`):

```
sub3402("bg_a", "menu/@/info_a");
sub3402("bg_b", "menu/@/info_b");
...
sub3402("bg_a", "menu/@/reset_a");
sub3402("bg_b", "menu/@/reset_b");
```

Damit ist klar: Der Menue-Hintergrund ist **nicht** `s_chlg_bg_a`
(`out/menu_top.bin`), sondern `menu/@/info_a` (Top) und
`menu/@/info_b` (Bottom). Die Dateien liegen als
`out/fa_uk/menu/uk/info_a.xi` (400x240) und `info_b.xi` (320x240) vor.

Der IMGC-Dekoder fuer diese Dateien ist aber noch nicht korrekt:
`info_a.xi` hat `s1=360`, die Tabelle dekomprimiert zu **0 Byte**,
der Texturstrom zu 27264 Byte (142 Tiles), benoetigt werden 1500 Tiles
(400x240 / 8x8). Deshalb bleibt das Bild bisher beim Platzhalter.



## Engine-GX-Submit laeuft: 0x12FD30 schreibt Queue, Trigger qops=1 (2026-11-23)

Der reale Submit war bisher **doppelt** abgeschnitten:

1. `0x12FD30` (echter Funktionsanfang: `push {r4..fp,lr}; bl 0x139914`)
   war durch `tools/patch_real_loader.py` auf `mov r0,#1; bx lr` gestubbt.
   `code_decomp.bin` enthielt also gar keinen Submit mehr.
2. Die Retprobe-Caves auf `0x124A14` und `0x124A40` hatten den Schwanz von
   `fn_0012493C` aus dem Recompiler-Blockgraphen entfernt; der Aufruf
   `bl 0x12FD30` bei `0x124ACC` existierte nicht in `gen_boot.c`.

Fixes (in `tools/build_port.py`, damit reproduzierbar):

- `0x12FD30/0x12FD34` Originalbytes `push {r4-r11,lr}` / `mov r4,r0`,
  `0x124A14` = `mov r4,r0`, `0x124A40` = `cmp r0,#0`.
- Seed `0012FD30` in `out/seeds.txt`, damit `fn_0012FD30` als eigene
  Funktion generiert wird.
- Neuer generischer Runtime-Hook **`HLE_RETFORCE=addr=value[,addr=value]`**:
  jeder generierte `fn_XXXXXX`-Einstieg ruft `arm_retforce(c, 0x...)` und
  kehrt bei Treffer mit `r0=value` zurueck (`runtime/arm_trace.c`,
  `tools/build_port.py`). Kein Rebuild pro Gate mehr.
- Neuer **`HLE_POKE=addr=value[,...]`** (32-Bit, pro Frame erneut) fuer
  Scratch-Strukturen/Stack-Argumente.
- `gsp_process_cmdqueue()` versteht zusaetzlich das von der Engine selbst
  geschriebene Queue-Layout: Anzahl `+0x18`, Listenadresse `+0x1C`,
  Groesse `+0xB0` (aus `fn_0013F2D4`/`fn_00139914`).
- `HLE_GSP_TRIGGER=1` ruft nach dem Engine-Submit den echten Engine-Wrapper
  `0x13E3E0` (`gsp::Gpu 0x000C`) auf.

Der erste Gate in `0x12FD30` (`0x139844`) laeuft **echt**: er liest das
SDK-Flag-Byte `0x3E7771`; das Wort `0x3E7770` ist im Port 0 und wird von
`hle_engine_frame()` auf `0x00010100` gesetzt. Auch der Groessenholer
`0x14015C` laeuft jetzt **ohne Force**: der Descriptor wird im Engine-Layout
angelegt (`[desc+4]=A`, `[A+0x3C]=C`, `0x143F78(C)=C`, `[C]=1`). Beleg:
`14015C` und `143F78` werden je 31x aufgerufen, `HLE_RETFORCE` ist im Lauf
nicht gesetzt.

Beleg (`python tools\run_engine_submit.py`, Log
`out/logs/engine_submit_run.txt`):

```
[gspq-engine] tid=0 n=1 list=0E800000 size=1000 ops=1
[queue] pre-trigger n=1 addr=0E800000 size=1000
[gsp] Trigger lr=0012FE1C addr=0CFE6000 size=24 qops=1
[hook-final] count addr=0012FD30 hits=31
[hook-final] count addr=00139914 hits=31
[hook-final] count addr=0014015C hits=31
[hook-final] count addr=00143F78 hits=31
[hook-final] count addr=0013F2D4 hits=31
[hook-final] count addr=0013A854 hits=31
[hook-final] count addr=0013E3E0 hits=66
[gsp] SetBufferSwap screen=0 fb=0x1F000000 stride=0x0 fmt=0
[gsp] SetBufferSwap screen=1 fb=0x1F060000 stride=0x0 fmt=0
```

Damit gilt erstmals: die **echte Engine-Submit-Funktion** `0x12FD30`
laeuft, ihre Kette `0x139914 -> 0x13F2D4 -> 0x13A854` ebenfalls, sie
schreibt den Queue-Eintrag in `0x10002800` (`[mw] 10002818 <- 1`), und der
Engine-Trigger `0x13E3E0` liefert `qops=1`.

### Ressourcen-/Menuepfad: noch vor dem `.xq`-Load

Die Menue-Sequenzen liegen im Archiv: `out/fa_a/seq/menu_logo.xq` (2648 B),
`menu_title.xq` (4840 B), `menu_bag.xq`, `menu_name.xq` usw. Der 300-Frame-
Lauf mit `HLE_ENGINE_MENU=1 HLE_MENU_OBJ0=1 HLE_DRAW_LIST=1` und
`HLE_HOOK=280744=count,28D424=count,2800C8=count,2919DC=count,28F7BC=count`
liefert **0 Treffer**; der Gast steht ab Frame 179 auf `pc=00100B94`
(`out/logs/res1_log.txt`). Der FA-Reader `0x280744` und der Draw-Builder
`0x2800C8` werden also noch nicht erreicht. Damit ist der `.xq`-Load der
naechste echte Ressourcen-Blocker (nicht der Submit).

Noch offen: die **Pixel** kommen im Standardlauf weiterhin aus dem
HLE-2D-BlitRGB-Ring (`lr=001ED9C8`, Assets `out/menu_top.bin`/
`out/menu_bottom.bin`; 0/96000 + 0/76800). Die echte Menuekomposition
(`menu_logo`/`menu_title`-Sequenzen aus `lt6_a.fa`) ist noch nicht
angesteuert; die Referenz `reports/menu_official*.bin` bleibt Vergleich.
Messung `out/shot_engine_submit.bmp`: gegen `out/menu_top.bin`/`menu_bottom.bin`
`0/96000` + `0/76800`, gegen `reports/menu_official.bin`/`_bottom.bin`
`96000/96000` + `76800/76800` (das Bild ist also noch die Asset-Komposition,
nicht das offizielle Menue).
Standardlauf und Tests unveraendert: `13/13`, `12/12`.


## Zwei Engine-Blocker behoben (2026-11-22, nachmittags)

Bis hierhin erreichte **kein** Lauf mehr einen gsp-Submit. Ursachen und Fixes:

1. **Fehlender Recompiler-Einstieg `fn_001ED64C`**: `fn_001ED64C` ist der
   Prolog des 2D-Renderer-Init (Rumpf ab `0x1ED65C`); der Recompiler hatte nur
   `fn_001ED65C` erzeugt. Der Gast ruft `0x1ED64C` aus Block `0x00285C48`;
   das endete mit `arm: unbekanntes call-Ziel 0x001ED64C`, der CPU-Thread
   starb (`pc=00100000`), danach lief kein Engine-Render mehr.
   Wrapper in `build/gen_boot.c` ergaenzt (frueher Ausstieg, sonst Rumpf);
   `tools/build_port.py` fuegt ihn nach jeder Regeneration wieder ein.
2. **gsp-Handle wurde zu frueh alloziiert**: `hle_init()` vergab den
   `gsp::Gpu`-Handle vor `memset(g_handles,0,...)`; der Slot wurde wieder frei,
   `g_hle_gsp_handle` blieb auf 1, `[0x3D9098]` auf 0. Allocation jetzt nach
   dem Reset (`srv-handle=1`, `gsp-handle=2`) plus Schreiben in den
   Client-Slot, den der Displaylist-Stub liest.

Messung `out/logs/final_gsp_log.txt` (120 Frames, `out/shot.bmp`):

```
[hle] romfs=out\romfs log=1 srv-handle=1 gsp-handle=2
[gsp] SetBufferSwap screen=0 fb=0x1F000000 stride=0x0 fmt=0
[gsp] SetBufferSwap screen=1 fb=0x1F060000 stride=0x0 fmt=0
[engine] 2D-Displaylistentry installiert (entry=0CFE3000 arr=14004440 gsp_h=2 slot=00000002)
[engine2d] Sprite-Entry idx=0..3 submitted (lr=001ED9C8)      # je 220x
[gsp] SetCommandList addr=0x0CFE6000 size=0x24 lr=001ED9C8
[gsp] TriggerCmdReqQueue -> 1 Ops (legacy)

SetCommandList=1760  Trigger=1760  SetBufferSwap(screen=0/1)=56
```

Pixelvergleich `out/shot.bmp`:

```
TOP    (0,0,400,240):    0/96000 Diff gegen out/menu_top.bin
BOTTOM (40,240,320,240): 0/76800 Diff gegen out/menu_bottom.bin
gegen reports/menu_official.bin: 96000/96000, unten 76800/76800
```

Der Engine-2D-Renderer submitted also nachweislich selbst, aber die
extrahierten Assets (`menu_top.bin`/`menu_bottom.bin`) sind **nicht** das
offizielle Menue: Korrelation der Rotkanaele nur 0.02, Bild ist ein
Farbverlaufs-Hintergrund ohne Text. Die Suche nach der echten
Menue-Komposition (2D-Layer bzw. 3D-Szene) bleibt offen.

## 3D-Renderer-Gates gefunden (2026-11-22, abends)

`fn_001B9938` (3D-Renderer) brach bisher bei **jedem** Aufruf sofort bei
`0x1BA964` ab. Ursache sind zwei Gates, die im Port nie gesetzt werden:

```
0x1B9958 -> 0x24B8C4: return *(s8*)(0x3D8E68 + 2)      # muss 0 sein
0x1B9964 -> 0x24B8AC: return *(s8*)(0x3D94E8) & ~*(s8*)(0x3D94E9=false)
                       # muss != 0 sein
```

Messung vorher: `1B9938 hits=62`, `1BA964 hits=62`, `34AD7C hits=0`.
Mit `[0x3D8E6A]=0`, `[0x3D94E8]=1`, `[0x3D94E9]=0` (in `HLE_ENGINE_3D`):

```
[hook-final] count addr=001B9938 hits=1
[hook-final] count addr=0034AD7C hits=1     # Renderpfad erreicht
[port] frames=299 pc=001BA674                # bleibt in 0x1BA6xx..0x1BA7B8
kein Submit aus lr=001BA3A0, kein SetCommandList im 3D-Lauf
```

Der Renderer betritt also seine echte Schleife, kehrt aber in 10 s nicht
zurueck (PC zyklisch in `0x1BA674`/`0x1BA7B8`). Beleg:
`out/logs/g3d_log.txt`, `out/logs/g3d2_log.txt`.

### Ursache: GPU-Busy-Spin am Block `0x1BA898`

```
0x1BA898  bl 0x24B8C4            ; liefert [0x3D8E6A] (GPU fertig?)
0x1BA8A0  beq 0x1BA364           ; solange 0 -> Schleife
```

Im Port ist `[0x3D8E6A]` dauerhaft 0 (kein asynchroner GPU-IRQ), also spinnt
`fn_001B9938` endlos. Da der Port synchron rendert, geht Block `0x1BA898`
jetzt direkt zu `0x1BA8A8` (Finalisieren). Der Patch steht dauerhaft in
`tools/build_port.py`.

Messung mit 300 Frames (`out/logs/3dok_log.txt`):

```
[hook-final] count addr=001B9938 hits=481
[hook-final] count addr=0034AD7C hits=481
[hook-final] count addr=001BA8A8 hits=481
[hook-final] count addr=0034ABB4 hits=481     # Finalisieren/Submit
SetBufferSwap(screen=0/1)=122
```

Der 3D-Renderer kehrt jetzt jeden Frame zurueck. Er hat aber **null
Render-Units**: der Per-Unit-Draw `0x1BA3A0` wird nie ausgefuehrt. Alle
3856 `SetCommandList` kommen weiterhin aus dem 2D-Ring (`lr=001ED9C8`).

**Korrektur der Listenadresse (gemessen):** Die Unit-Liste liegt **im
Renderer-Objekt `0x00422200`**, nicht in der Scene und nicht bei
`0x455E68`:

```
Node-Array: 0x00422200 + 0x14F0   (Stride 0x20)
Count:      0x00422200 + 0x1530
```
Hergeleitet aus `0x1BA214..0x1BA288` (`arg+0x1000+0x4F0`, `+0x1000+0x530`)
und `0x1BA378` (`ldr r0,[fp]`, `add r4,r4,#0x20`). `0x455E68` ist nur ein
Matrix-/Kamera-Block (`+0x00/+0x10/+0x20`, per `fn_0027B9A8` gesetzt).

Laufzeit-Beleg (`out/logs/scene3_log.txt`):

```
[engine3d] renderer=00422200 units=0 node0=00000000 type=00000000 (scene=003CA494 scene_units=0)
```

Der Unit-Schreibpfad ist `fn_0011AEF4`: er berechnet
`0x422200+0x1000+0x4F0 + index*0x20` und schreibt die 0x20-Byte-Nodes.

### Menue-Setup laeuft jetzt (HLE_ENGINE_MENU)

Einstiegskette des Spiels:

```
0x10001C (fn_00100000) -> fn_00100B4C -> fn_00100F98   (Menue-Setup, r0 = Byte-Struct mit Sprachindex)
   -> fn_001034F4(world=0x003EC028, scene)
   -> fn_00108F70 -> fn_00285208   (Welt-Update)
```

`HLE_ENGINE_MENU=1` ruft `fn_00100F98` einmal (Byte `[arg]=0`). Messung
mit 120 Frames (`out/logs/menud_log.txt`):

```
[enginemenu] fn_00100F98(0CFE9000) -> ...
[hook-final] count addr=002800C8 hits=6176    # Draw-Liste
[hook-final] count addr=0028D424 hits=6175    # Menue-Ressourcen-Loader
[hook-final] count addr=00285208 hits=1       # Welt-Update
[port] frames=149 pc=002770D8 units=0
```

Der Aufruf kehrt nicht zurueck: er laeuft in der **echten Menue-Schleife**
des Spiels (`0x2770D8`, `0x28D43C`). Das Welt-Update `fn_00285208` laeuft
dabei genau einmal, aber `[0x422200+0x1530]` bleibt **0** (Host-Auslesung
in `[port] frames=`); waehrend dieser Schleife gibt es **0** gsp-Submits.
Der naechste Blocker ist damit die 3D-Drawable-Erzeugung der Menue-Szene,
nicht mehr der Aufrufpfad.

### Draw-Liste wird doch gefuellt (nur zwischen zwei Samples)

Write-Watch auf `0x3EE188..0x3EE1B8` (`out/logs/wr_log.txt`) und auf den
Array-Bereich (`out/logs/arr_log.txt`) waehrend der Menue-Schleife:

```
[mw] 0CFF0000 <- 0CFF2100 size=4 pc=002802D4 lr=00288908   # Element
[mw] 003EE1A0 <- 00000001 size=2 pc=002802D4 lr=00288908   # Count 0 -> 1
[mw] 003EE1A0 <- 00000000 size=2 pc=0028DED0 lr=0028DE78   # Reset
```

`fn_002800C8` schreibt also pro Frame genau einen Eintrag
(`0x0CFF2100`) in den Array `0x0CFF0000` und setzt den Count auf 1; eine
spaetere Stelle (`0x28DED0`) setzt ihn wieder auf 0. Der 30-Frame-Sampler
sieht deshalb meist 0.

### Transfer in die 3D-Unit-Liste: units=1 erreicht

`HLE_UNIT_XFER=1` spiegelt diesen Draw-List-Eintrag nach
`0x004236F0` (Node 0) und setzt `[0x00423730]=1`, dann wird
`fn_001B9938` aufgerufen. Host-Beleg (`out/logs/xfer_log.txt`):

```
[unitxfer] obj=0CFF2100 -> units=1, 3D-Renderer aufgerufen
[port] frames=179 ... units=1
```

Der Per-Unit-Draw `0x34AFAC` wird aber weiterhin nicht beobachtet
(`ARM_WATCH=34AFAC` -> 0 Treffer), und es gibt **keinen** neuen
`SetCommandList` (192, alle `lr=001ED9C8`).

Zusatz-Diagnose direkt im recompilierten `fn_001B9938`
(`build/gen_boot.c`: `[entrydbg]`, `[flowdbg]`, `[unitloop]`):

```
[entrydbg] r0=00422200 en=1        # Enable-Gate [0x4237A3] ist gesetzt
[unitxfer] ... units=1
(kein [entrydbg] early-exit 1BA964)
(kein [flowdbg] block=0x1BA364/438/454)
(kein [unitloop])
```

Der Renderer wird also mit `r0=0x00422200` und aktivem Enable aufgerufen,
bricht **nicht** am Early-Exit ab, erreicht aber den Unit-Block
`0x1BA364` gar nicht - er laeuft auf einem anderen Pfad (Modus-/State-
Abhaengigkeit im Renderer-Objekt). Deshalb ist der Node-Typ/Format noch
nicht die einzige Luecke.

Hinweis: Die Debug-Prints stehen derzeit in `build/gen_boot.c`; bei
`build_port.py` muessten sie erneut eingefuegt werden.

### Unit-Block erreicht und `0x34AFAC` laeuft (2026-11-22)

Ursache war der fruehere Spin-Patch: er hatte den **einzigen** Sprung nach
`0x1BA364` (`0x1BA898: beq 0x1BA364`) auf `0x1BA8A8` umgebogen. Jetzt:

- Branch wiederhergestellt (`beq -> 0x1BA364`).
- Im Dispatcher-Block `0x1BA634` wird beim ersten Durchlauf
  `[0x3D8E6A] = 1` gesetzt, damit der GPU-Wait danach endet.

Messung (`out/logs/unit_log.txt`, `out/logs/uw_log.txt`):

```
[pathdbg] block=001BA364 r0=0
[unitloop] count=1 node=004236F0
[watch] 0x0034AFAC r0=0CFF2100 r1=00000001 r2=00000000 r3=00422DF8 r4=004236F0
[watch] mem@r4: 0CFF2100 00000000 ...
```

Der Engine-Pfad `0x1BA364 -> Unit-Loop -> 0x34AFAC` laeuft also erstmals.
Es gibt aber weiterhin **keinen 3D-gsp-Submit** (192 `SetCommandList`, alle
`lr=001ED9C8`).

**0x34AFAC ist nur ein Handle-/Sync-Helfer:** Es ruft `0x34B97C`,
schliesst ein evtl. vorhandenes Handle (`svc 0x23` = CloseHandle) und
schreibt das Ergebnis nach `[obj]`. Die eigentliche Unit-Arbeit folgt im
Block danach ueber `0x24B258 -> 0x34B55C` (mit `r2 = node+0x18`,
`r3 = Tabelle`, `r1 = node+0x10`).

**Kein GSP-Schreibzugriff:** `ARM_MEMWATCHRANGE=10002800,400`
(CommandBuffer) zeigt waehrend des Unit-Durchlaufs **0** Writes
(`out/logs/gw_log.txt`). Die Engine erzeugt mit dem synthetischen Objekt
`0x0CFF2100` also noch keine PICA-Kommandoliste.

### Die Unit-Helfer sind cam:u-Aufrufe, nicht gsp

`HLE_IPC_RAW=1` + `HLE_HDL_LOG=1` (`out/logs/raw_log.txt`, `hdl_log.txt`):

```
[hdl] 1 = srv:
[hdl] 2 = gsp::Gpu
[hdl] 4 = cam:u
[hdl] 5 = cam:u,

[ipcraw] h=4 hdr=00060040 cmd=0006 ... blk=0034B97C lr=0034AFC4
[ipcraw] h=4 hdr=00070102 cmd=0007 ... blk=0034B55C lr=0024B294
```

`0x34B97C`/`0x34B55C` senden also an **cam:u** (Handles 4/5), nicht an
`gsp::Gpu`. Der „Typ-Tabellen"-Pointer bei `0x1BA980` zeigt auf
`0x3AF3F0` (Bytes `1,2,0,1,2,3,4,...` direkt vor dem String `"cam:u"`):
Typ 0 wird zu 1, und der Unit-Zweig nimmt den Kamera-Pfad. Deshalb gibt es
keinen 3D-gsp-Submit mit dem synthetischen Node.

### Der Engine-GSP-Client laeuft gar nicht

Zwei Messungen im laufenden Menue-Loop (120 Frames):

```
ARM_MEMWATCHRANGE=10002000,2000   -> 0 Writes (out/logs/gspw_log.txt)
Hooks 0x138D70 (GSP-Init), 0x13E3E0 (Trigger) -> 0 Treffer (gh_log.txt)
```

Der Engine-GSP-Client initialisiert also nie, und der Trigger wird nie
gerufen; die Engine schreibt den GSP-Shared-Memory `0x10002000` nicht an.

**Client-Init jetzt gueltig:** `HLE_GSP_ENGINE=1` schreibt vor dem Aufruf
von `0x138D70` den Namen `"gsp::Gpu"` nach `0x3D91F8` und Laenge 9 nach
`0x3D91EC` (`client+4`). Danach (`out/logs/gsi_log.txt`):

```
[gspengine] 0x138D70: h10=4 h14=4 state=003D7C40
```

Der Client (`0x3D91E8`) hat also ein echtes Handle (4) und den State-Zeiger
`0x3D7C40` in `+0x1C` - wie im echten Boot.

**Trigger laeuft, schreibt aber nichts:** `0x13E3E0` wird mit
`r0=0x3D7C40` periodisch aufgerufen (`ARM_WATCH=13E3E0`: 11 Treffer),
aber `ARM_MEMWATCHRANGE=10002000,2000` bleibt bei **0 Writes**
(`out/logs/trig_log.txt`), und es gibt keinen neuen `SetCommandList`.

### Der Engine-Wrapper benutzt cam:u-Handles

Der gemeinsame IPC-Wrapper `0x34B97C`/`0x34B55C` holt sein Handle aus
`[0x3DA6DC]`. Die Engine schreibt dort selbst **cam:u**-Handles hinein
(`HLE_HDL_LOG=1`, `out/logs/gh2_log.txt`):

```
[hdl] 7 = cam:u
[hdl] 8 = cam:u
[ipcraw] h=7 hdr=00060040 cmd=0006 ... blk=0034B97C
[ipcraw] h=8 hdr=00070102 cmd=0007 ... blk=0034B55C
```

Ein erzwungener gsp-Handle im Client (`h10=... -> gsp=2`,
`out/logs/gfix_log.txt`) hilft nicht, weil `[0x3DA6DC]` vom Spiel kurz
vorher mit cam:u ueberschrieben wird.

### gsp-Handle im Unit-Block erzwingen: Engine sendet gsp-IPC

Im Unit-Block (`0x1BA390`) wird jetzt `[0x3DA6DC] = g_hle_gsp_export`
geschrieben (gsp-Handle, in `hle.c` exportiert). Messung
(`out/logs/gslot_log.txt`, `gi2_log.txt`):

```
[ipcraw] h=2 hdr=00060040 cmd=0006 ... blk=0034B97C lr=0034AFC4
[ipcraw] h=2 hdr=00070102 cmd=0007 ... blk=0034B55C lr=0024B294
[gsp] RequestDma p0=00000000 p1=00000001           (1305x)
[gsp] SetCommandList addr=0x00000001 size=0x0 lr=0034AFC4  (2x)
[ipcraw] h=1 hdr=000C0040 cmd=000C ... blk=0013E3E0 lr=001BA5A4  (9x)
```

Erfolge: Der Engine-Wrapper sendet jetzt an **gsp (h=2)**; RequestDma
kommt 1305×, und der Engine-Trigger `0x13E3E0` ruft
`TriggerCmdReqQueue` (cmd 0x000C) auf.

**Aber die 0x34Bxxx-Wrapper sind cam:u-Aufrufe:** `0x34B97C` schreibt
Header `0x00060040` und **nur einen** Normal-Parameter (Byte von
`[sp+4]`); das ist keine gsp-`SetCommandList`-Signatur (Adresse+Size),
sondern eine cam:u-Kommandoform. Mit erzwungenem gsp-Handle parst der
HLE sie deshalb als `addr=1, size=0`.

**Echte Submit-Wrapper laufen nicht:** Hooks auf die dokumentierten
Pfade `0x1196AC` (0x0006), `0x120DA0`/`0x1263B8` (0x000C), `0x275B24`
(0x0005) ueber 120 Frames der Menue-Schleife: **0 Treffer**
(`out/logs/hs_log.txt`). Shared-Memory `0x10002000` bleibt 0 Writes.

### Uebersicht der IPC-Header im Code

Byte-Scan (`tools/_scan_hdrs.py`):

```
0x00060040 : 0x119264 (gsp-Wrapper, 1 Param), 0x11AB80, 0x34B97C (cam)
0x000C0040 : 0x1232CC, 0x13E3E0 (generischer IPC-Wrapper), 0x256E40,
             0x3418BC, 0x341D34
0x000600C4 / 0x000C00C4 : 0 Treffer (keine Translate-Variante im Code)
```

### Engine-Trigger erreicht jetzt gsp

`0x13E3E0` ist ein generischer IPC-Wrapper: `r0` zeigt auf die
Handle-Variable (`ldr r0,[r0]`), `r1` ist der Parameter, `r2` der
Out-Zeiger. Mit einem Scratch-Slot `0x0CFE9100`, der den gsp-Handle
enthaelt, geschieht (`out/logs/trg2_log.txt`):

```
[ipcraw] h=2 hdr=000C0040 cmd=000C ... blk=0013E3E0 lr=001BA478
[gsp] TriggerCmdReqQueue -> 1 Ops (legacy)   (mehrfach)
```

Der Engine-Trigger erreicht also jetzt `gsp::Gpu` (h=2), auch aus dem
Renderer-Unit-Pfad (`lr=0x1BA478`). Shared-Memory bleibt 0 Writes, und
ein echtes `SetCommandList` mit Adresse+Size fehlt weiterhin.

Nachtrag: Der Client-State-Init `0x13EAAC` speichert das Ergebnis von
`0x28771C` nach `[state+0x2C]`; im Port ist das **5** (kein
Shared-Memory-Zeiger) - `[gspengine] shm(base+0x2C)=00000005`
(`out/logs/shm_log.txt`). Wo die Engine ihren Kommandopuffer ablegt, ist
damit noch offen.

### Wichtig: der Engine-Trigger spielt unsere HLE-Liste erneut ab

Der HLE-Handler fuer `TriggerCmdReqQueue` (0x000C) fuehrt **immer** die
zuletzt per `SetCommandList` gesetzte Liste (`g_cmdlist_addr/size`) aus,
wenn sie bekannt ist (`runtime/hle/hle.c`, `case 0x000C`). Da das
Engine-`SetCommandList` degeneriert ankommt (`addr=1,size=0`), bleibt
`g_cmdlist` auf unserem 2D-BlitRGB-Pfad `0x0CFE6000` stehen; der
Engine-Trigger rendert damit erneut die HLE-Liste (Log
`TriggerCmdReqQueue -> 1 Ops (legacy)`). Deshalb bleibt das Bild trotz
Engine-Trigger identisch zu den Assets.

Konsequenz: Entweder muss das Engine-`SetCommandList` echte
`addr`/`size` liefern (dann laeuft eine Engine-Liste), oder der
Shared-Memory-Ring `0x10002800` muss vom Engine-Pfad gefuellt werden,
bevor `gsp_process_cmdqueue` ihn liest.

### Shared-Memory-Ring vor dem Trigger fuellen: Engine-Trigger konsumiert ihn

Neu: `HLE_RING_FILL=1` schreibt vor dem Engine-Trigger
(`0x13E3E0`, h=2) einen `SubmitCmdList`-Eintrag nach `0x10002800`
(Kopf `1<<8`, Slot +0x20: id=1, addr=`0x0CFE6000`, size=0x24);
`HLE_RING_ONLY=1` ueberspringt den Legacy-Replay, wenn `qops>0`.
Messung (`out/logs/ring2_log.txt`, 60 Frames):

```
[gsp] Trigger: 1 Ops aus Shared-Memory-Ring   (9x)
[gsp] Trigger lr=001ED9C8 addr=0CFE6000 size=24 qops=0   # 2D-Ring (Legacy)
```

Damit ist belegt: Der **Engine-Trigger** (`0x13E3E0`, gsp h=2) liest den
Shared-Memory-Ring und fuehrt die dortige `SubmitCmdList` aus (9x, je 1 Op).
Die Pixel bleiben assetgleich, weil der Eintrag noch auf die HLE-Liste
`0x0CFE6000` zeigt; fuer ein Engine-Bild muss dort eine echte
PICA-Liste stehen.

**Isolation:** `HLE_RING_ONLY=1` allein aendert nichts (`TOP 0/96000`,
`BOTTOM 0/76800`, `out/logs/ro_log.txt`): Die 2D-Ring-Trigger haben
`qops=0`, also greift weiterhin der Legacy-Replay. Nur die per
`HLE_RING_FILL` erzeugten Engine-Trigger (qops=1) ueberspringen ihn.
Das Bild wird derzeit also vom Legacy-Pfad gemalt, nicht aus dem Ring.

### Engine-Objekte enthalten echte Methoden-Zeiger (Dump)

`HLE_MENU_WORLD` dumpt Menue- und Scene-Objekt (`out/logs/dump_log.txt`):

```
[menudump] 3EE12C: 003C6FE4 00000000 003EC028 ... 00000014 ...
[menudump] scene:  003653CC 00000000 0010F09C 0036C6E8 00000000 00000000
                   0036C54C 0036C60C ... 0036CBD0 ... 00273908 0037EF94 0037F7F4
```

Die Zeiger `0x003653CC`, `0x0036C6E8`, `0x0036C54C`, `0x0036C60C`,
`0x0036CBD0` sind **Engine-Funktionen** (Beginn jeweils mit `push ...`);
`0x0037EF94` ist die bekannte Readiness-Funktion. Das Scene-Objekt hat
also echte Methoden-Zeiger, die als Draw-Methoden fuer einen
Ring-Eintrag dienen koennen.

### Echte Engine-Draw-Methode als Ring-Eintrag (HLE_REAL_ENTRY)

`HLE_REAL_ENTRY=1` setzt die Draw-Methode des Displaylist-Eintrags auf
einen Host-Stub `hle_engine_entry` (registriert unter `0x0CFE4000` in
`g_entries`), der `r0 = 0x003CA494` (Scene) setzt und die Engine-Funktion
`HLE_ENTRY_FN` (Default `0x0036C6E8`) aufruft.

Messung (`out/logs/re2_log.txt`, 60 Frames):

```
[watch] 0x0036C6E8 r0=003CA494 r1=0CFE4000 r2=14004440 r3=00000000
unbekanntes call-Ziel: 0
SetCommandList: 0      (Engine-Funktion submitted nicht ueber gsp)
Screenshot: TOP 95368/96000, BOTTOM 70954/76800 Diff zu den Assets
            (einfarbig 96,64,32)
```

Damit ist erstmals eine **echte Engine-Funktion als Draw-Methode**
ausgefuehrt worden, und das Bild ist nicht mehr assetgleich. Sie
submittet aber noch keine gsp-Liste.

### Matrix: alle Engine-Methoden getestet (`tools/_fn_matrix.py`)

```
36C6E8 exit=0 sets=0 unk=0 TOP=95368 BOTTOM=70954
36CBD0 exit=0 sets=0 unk=0 TOP=95368 BOTTOM=70954
36C54C exit=0 sets=0 unk=0 TOP=95368 BOTTOM=70954
36C60C exit=0 sets=0 unk=0 TOP=95368 BOTTOM=70954
3653CC exit=0 sets=0 unk=200 TOP=95368 BOTTOM=70954
```

Alle vier registrierten Methoden laufen ohne unbekannte Call-Ziele, aber
**keine** setzt `SetCommandList` ab; das Bild ist jeweils der HLE-Present-
Clear (einfarbig), nicht die Engine-Ausgabe. `0x3653CC` ist kein
registrierter Einstieg (`unk=200`). Die Methoden brauchen also weitere
Argumente/Zustand (nicht nur `this = 0x3CA494`).

### Aufrufkonvention von `vtable+8` (aus `fn_001ED8FC`)

```
0x1ED9AC  bl 0x284A38        ; r0 = &ring[index]  (Array 0x14004440, Stride 0xB4)
0x1ED9BC  ldr r1,[r0]        ; vtable
0x1ED9C0  ldr r1,[r1,#8]     ; Draw-Methode
0x1ED9C4  blx r1             ; Aufruf, r0 = Eintrag-Zeiger
```

Der Renderer uebergibt also **nur `r0` = Zeiger auf den Ring-Eintrag**;
`r1/r2/r3` sind Reste aus `0x284A38`. Ein echter Engine-Eintrag muss
demnach ein Objekt an `r0` liefern, dessen Felder die Draw-Methode liest
(unsere Engine-Methoden lesen z. B. `[r0+0xA34]`) - der Ring-Eintrag ist
nur 0xB4 Byte gross, also muessen die echten Eintraege per eigenem Stub
`r0 = [entry+4]` (Objektzeiger) laden.

**Implementiert + gemessen:** `hle_engine_entry` liest jetzt
`r0 = [entry+4]`; der Installer schreibt `entry+4 = HLE_ENTRY_OBJ`
(Default `0x3CA494`). Matrix ueber Kandidaten-Objekte mit
`HLE_ENTRY_FN=36C6E8` (`tools/_obj_matrix.py`):

```
3CA494/3EE12C/3C6FE4/3EC028/3EE188/3CA4A0:
sets=0 unk=0 TOP=95368 BOT=70954 lr=[]
```

Kein Kandidat erzeugt eine `SetCommandList`; das Bild bleibt der
HLE-Present-Clear. Die getesteten Scene-Methoden sind also nicht der
Sprite-/Mesh-Renderpfad - die echten Drawable-Objekte (Sprites) fehlen
weiterhin.

### Write-Watch: das Spiel fuellt den Drawable-Pool nie

`ARM_MEMWATCHRANGE=3EE188,20` im Menue-Loop ohne `HLE_DRAW_LIST`
(`out/logs/poolw_log.txt`): nur BSS-Clear (`pc=0x100030`) und der
Konstruktor (`pc=0x288398`) schreiben in den Pool. **Keine** Spiel-
Schreibzugriffe auf `{array,stride,start,end}` (`menu+0x5C..`) waehrend
120 Frames - der Menue-Loop erzeugt also selbst keine Drawables. Das
erklaert `sets=0` fuer alle Kandidaten-Objekte.

Statische Suche nach dem Add-Writer (`tools/_find_pool_writers.py`):
569 Treffer fuer `+0x5C`, 27 fuer `+0x6A` - viele davon sind aber
gefaltete Offsets (z. B. `add r0,r0,#0x100` + `strh [r0,#0x6A]` =
`obj+0x16A`, siehe `fn_001C3950`). Ein eindeutiger Add-Drawable-Aufruf
fuer `0x3EE12C+0x6A` ist damit noch nicht identifiziert.

Write-Watch auf das ganze Menue-Objekt (`0x3EE12C..0x3EE1AC`,
`out/logs/mo_log.txt`): Der Loop schreibt nur das Flag `+0x7C`
(`0x3EE1A8`, 162x toggelnd, pc `0x28F078`/`0x28DE28`). Einmalig werden
`menu+0x00` auf `0x003C4F4C` bzw. `0x003C7548` und `menu+0x08` auf
`0x003EC028` gesetzt (pc `0x288890`). Diese beiden Objekte wurden als
`HLE_ENTRY_OBJ` mit `0x36C6E8`/`0x36CBD0` getestet: `sets=0`, `unk=0`,
Bild unveraendert (`out/logs/x_<obj>_<fn>.txt`). Sie sind also ebenfalls
keine Sprite-Drawables.

### Menue-State-Objekt `0x3C6FE4` (vtable-artig) und seine Methoden

Dump (`out/logs/st_log.txt`):

```
state=003C6FE4: 00000000 00000000 0011DB84 ... 00231868 0037F948
                0037F910 00376F98 0037F9BC 00376F70 0037FBC8
                00298730 0029880C 0029BB78 ...
```

Das ist eine Tabelle von Engine-Funktionszeigern. Als Draw-Methode mit
`this = 0x3C6FE4 bild` getestet (`tools/_vt_matrix.py`):

```
11DB84 unk=142 sets=0 ; 37F948/37F910/376F98/37F9BC unk=200 sets=0
37FBC8 unk=0 sets=0
```

Die meisten Ziele sind nicht als Einstieg registriert; keine Variante
setzt `SetCommandList` ab.

### Wichtige Entdeckung: `menu+0x6C/+0x74` ist die Node-Liste

`0x28D424` ist eine **State-Machine ueber Node-Objekte**:

```
0x28D43C  ldrh r1,[r4,#0x74]     ; Node-Count  (= 0x3EE1A0!)
0x28D46C  ldr  r0,[r4,#0x6C]     ; Node-Array  (= 0x3EE198!)
0x28D470  ldr  r5,[r0]           ; node = array[i]
0x28D47C  ldrsb r0,[r5,#8]       ; Typ 3..7 waehlt Zweig
```

Damit sind die von `HLE_DRAW_LIST` benutzten Felder `0x3EE198/0x3EE1A0`
genau Array/Count der Menue-Node-Liste. Test mit einem synthetischen
Node (`HLE_MENU_NODE=<3..7>`, Objekt `0x0CFF2100`, Typ an `+8`):

```
type 3: sets=192 (nur lr=001ED9C8) unk=47 TOP=0 BOT=0
type 4: sets=192 unk=41 ; type 5: unk=60 ; type 6/7: unk=21
```

Die Typ-Zweige laufen an, rufen aber (mangels echter Objekt-Methoden)
ins Leere (`unbekanntes call-Ziel`), und das Bild bleibt assetgleich.

**Was die Zweige wirklich tun (Disassembly):**

```
Typ 3 (0x28D4D4): nur Bit-Rotation auf [node+8]+0x14, kein Aufruf
Typ 5 (0x28D574): sucht Objekt in menu+0x38 (Liste, Count +0x40) mit
                  [obj+8]->[+8] == [node+0x10]; dann
                  r1 = [obj]; r1 = [r1+0x34]; blx r1     <- Engine-Methode
Typ 6 (0x28D5D8): derselbe Suchlauf, weiterer Zweig mit [obj+8]+0x19
```

Damit ist die Engine-Objektform bekannt: Objekte in `menu+0x38`, deren
`[obj]` eine Vtable mit Methoden u. a. bei **`+0x34`** ist (und `+0x08`
fuer den 2D-Renderer). Diese Objekte/Listen sind im Port noch leer; der
Typ-5/6-Pfad ruft deshalb ins Leere.

**Write-Watch `0x3EE164..0x3EE174` (`menu+0x38/+0x40`)** ueber 120 Frames
(`out/logs/m38_log.txt`): nur BSS-Clear (`pc=0x100030`) und Konstruktor
(`pc=0x288398`) schreiben. Das Spiel registriert **keine** Objekte in
`menu+0x38` - Menue-Node-Liste **und** Objektliste sind im Port leer.
Damit ist klar: Nicht der Renderer, sondern die **Befuellung des
Menue-Objekts** fehlt.

Das Spiel ruft `0x28D424` selbst mit `r0=0x003EE12C` auf
(`ARM_WATCH=28D424`, `out/logs/w28_log.txt`); ein Zeiger auf
`0x3EE12C` existiert aber nirgends im RAM (`[menuscan] ptr(3EE12C):`
leer) - die Adresse wird berechnet (Basis `0x3EC028` + Offset).

**Aufrufkette des Menue-Loops:**

```
fn_0011DB84(r0=Menue)  ->  fn_00277004 (Menue-Drain: 0x28D424/0x2800C8)
fn_0035FF14 (Scene-Builder, kein statischer Aufrufer)
    0x3610C0..0x3610D8  bl 0x265DD0   ; Container-Traversal
    0x3610E8  sub r4,r0,#0x80         ; r4 = &menu (eingebettetes Member)
    0x3610F8  bl 0x277004
```

`0x3C6FE4+8` ist genau `fn_0011DB84` - das State-Objekt ruft also den
Menue-Drain. Das Menue-Objekt ist ein **eingebettetes Member** der Scene
(Adresse ueber Container `0x265DD0` berechnet), daher kein Literal und
kein Pointer im RAM. Ohne die echte Scene-Konstruktion (`fn_0035FF14`)
bleibt es unbefuellt.

**Echter Einstieg gefunden:** Push-Prolog-Scan (`tools/_findfn.py`) zeigt, dass der Builder bei **0x0035F3B4** beginnt (nicht 0x35FF14; 0x35FF14 liegt mitten im Code).

**Aufrufversuch `HLE_SCENEBUILD=1`:** `fn_0035FF14(...)` wird mit
`0x3CA494`, `0x3EC028` bzw. `0x3EE12C` aufgerufen; es erscheinen mehrere
`unbekanntes call-Ziel 0x00000000` (Blk `0x26DC44`, `0x265940`,
`0x123D74`, `0x36714C`) und der Aufruf kehrt nicht zurueck
(`out/logs/sb_log.txt`, `sb2_log.txt`). Das bestaetigt: der Builder
braucht echte Container/Sub-Objekte, die im Port fehlen.

**Objektliste-Befuellung getestet:** `HLE_MENU_NODE=<5|6>` fuellt jetzt
zusaetzlich `menu+0x38/+0x40` mit dem Engine-State-Objekt
(`HLE_MENU_OBJ`, Default `0x3C6FE4`). Ergebnis (`out/logs/ol_*.txt`):

```
type 5 / obj 3C6FE4: sets=192 (nur lr=001ED9C8) unk=56 TOP=0 BOT=0
type 5 / obj 3CA494: sets=192 unk=31 TOP=0 BOT=0
type 6 / obj 3C6FE4: sets=192 unk=41 TOP=0 BOT=0
type 6 / obj 3CA494: sets=192 unk=30 TOP=0 BOT=0
```

Die Zweige laufen, rufen aber weiterhin in leere Sub-Objekte
(`unbekanntes call-Ziel`); kein Engine-Submit.

### Draw-Liste waehrend der Menue-Schleife: Pending-Pool waechst, Liste nicht

Mit `HLE_ENGINE_MENU=1 HLE_DRAW_LIST=1` und erweiterter Host-Auslesung
(`tests/port.c`, Log `out/logs/dl2_log.txt`, 200 Frames):

```
[port] frames=29  units=0 dl=0/256 arr=0CFF0000 e0=00000000 mn=1 pool=0CFF1000 0..1
[port] frames=59  units=0 dl=0/256 arr=0CFF0000 e0=0CFF2100 mn=0 pool=0CFF1000 0..2
[port] frames=179 units=0 dl=0/256 arr=0CFF0000 e0=0CFF2100 mn=0 pool=0CFF1000 0..2
```

Bedeutung: `fn_002800C8` **konsumiert** den Pending-Pool (`start..end`
waechst von `0..1` auf `0..2`, `[0x3EE196]` 1 -> 0), aber der beobachtete
Draw-List-Array `0x3EE198`/Count `0x3EE1A0` bleibt `0/256`. Die eigentliche
Ausgabe des Builders liegt also an einer anderen Stelle; `units` bleibt 0.

### Welt-Update periodisch in der Menue-Schleife: units bleiben 0

`HLE_MENU_WORLD=1` ruft aus `hle_pump` heraus (eigener Stack
`0x0CF0A000`, damit die Menue-Frames nicht ueberschrieben werden) alle
0x2000 Pumps `fn_00285208(0x003EC028)` und danach `fn_001B9938(0x00422200)`
mit geoeffneten Gates auf. Messung (`out/logs/mw2_log.txt`, 200 Frames):

```
[menuworld] units=0
[port] frames=179 pc=0028D43C units=0
gsp-Submits = 186                    # nur der 2D-Ring VOR der Menue-Schleife
arm: unbekanntes call-Ziel 0x00000000 (Block 0x0028BE0C)
arm: unbekanntes call-Ziel 0x00000000 (Block 0x0028EF94)
```

Der Welt-Update laeuft also jetzt *innerhalb* der Menue-Schleife, erzeugt
aber weiterhin **0** Render-Units; die Null-Calls zeigen fehlende
initialisierte Subsysteme im World-Objekt (`0x003EC028`). Der 3D-Renderer
kann daher nichts submitten.

### Warum units=0 bleibt: der Welt-Update-Pfad laeuft nie (Standardlauf)

Kette (gemessen/gelesen):

```
fn_00285208(world)  ->  fn_0011AEF4 (r0 := 0x00422200)  ->  fn_0027AF48
                        (Renderer-Begin: raeumt Units, 0x27B200 schreibt Nodes)
Aufrufer von fn_00285208: fn_00108F70, fn_0010F0B4, fn_0035F3B4
```

Hook-Lauf ueber 40 Frames (`out/logs/w_log.txt`): registriert sind 5
Adressen, Treffer hat nur der HLE-Aufruf:

```
[hook] 5 Adresse(n) registriert (HLE_HOOK)
[hook] count addr=001B9938 hits=1 ... 60
(0x00285208, 0x0011AEF4, 0x0027AF48, 0x0027B200: keine Treffer)
```

Das Spiel ruft seinen eigenen 3D-Welt-Update also **nie** auf; deshalb
bleibt `[0x422200+0x1530]=0`. Ein erzwungener Aufruf
(`HLE_ENGINE_WORLD=1`, `HLE_WORLD_ARG=<ptr>`) mit `0x003CA494` oder
`0x00422200` bricht mit `unbekanntes call-Ziel 0x00000000 (Block
0x0028EF94)` ab - das echte World-Objekt fehlt (Menue-Flow nicht erreicht).

## Offline-IMGC-Dekoder ist fuer den Menue-Atlas falsch

`out/x_menu/000.xi` (aus `s_menu_ani_a.xa`) enthaelt drei IMGCs; das
512x256-24bpp-Bild hat eine Tabelle mit **2048 eindeutigen** Eintraegen der
Form `block*4096 + row*16 + col` (`block` 0..7, `row`/`col` 0..15). Der
Kuriimu-Ansatz `offset = entry * bytesPerTile` liefert deshalb **schwarz**
(`_imgc.decode`), und auch die Permutationsvarianten
(`s = (e>>12)*256 + (e&0xFFF)`) ergeben nur Rauschen (Korrelation 0.01-0.11).
Der entpackte Texturstrom selbst ist ein Farbverlauf (Korrelation 0.08).

Damit sind die frueheren Aussagen "kein Asset matcht die Referenz" **nicht
belastbar** - sie beruhen auf einem falschen Dekoder. Werkzeuge:
`tools/_list_imgc.py`, `tools/_show_blob.py`, `tools/_menu_decode3.py`,
`tools/_title_etc1.py`, `tools/_scan_imgs.py`.

## Engine-Render + Dual-Screen (2026-11-22, laufender Stand)

Umgesetzt und im Port aktiv:

1. **Fenster 400x480**: Top 400x240 bei `y=0`, Bottom 320x240 bei
   `x=40,y=240`. `tests/port.c` komponiert beide Regionen direkt aus
   `hle_gsp_fb_addr(0)` / `hle_gsp_fb_addr(1)`; der Referenzpfad ist nur
   noch Debug-Fallback (`PORT_NO_ENGINE=1`).
2. **Framebuffer-Adressen**: `gsp_fb_info()` liest die von der Engine in den
   GSP-Shared-Memory geschriebene `FrameBufferInfo` (Azahar-Layout,
   `0x10002000+0x200+(2*thread+screen)*0x40`). Das Spiel sendet
   `SetBufferSwap screen=2` ("beide"); der HLE-Handler expandiert das zu
   **zwei** echten Swaps `screen=0` und `screen=1` mit den FB-Adressen.
3. **Engine-Frame**: `hle_engine_frame()` (env `HLE_ENGINE_FRAME`) taktet den
   echten Dispatcher `fn_0028F084(0x003EC028)` sowie `fn_00103D20`; der
   2D-Scene-Renderer `fn_001ED8FC` (`0x366B98`, globales Objekt `0x408808`)
   wird zusaetzlich direkt aufgerufen (`HLE_ENGINE_2D`).
4. **Gates**: `[0x40B354]` (0x11AF10), `[0x40B3A0]` (0x37FB1C),
   `[0x3D8D5D]`, `[0x3D8D64/65]` (0x119D10), `[0x3EE16C]` (Scene+0x2144),
   `[0x40B4CC]=0x00101750`, `[0x4237A3]` (Renderer+0x15A3). Der im Port nie
   aufgerufene Scene-Konstruktor `fn_00381CE4` wird einmal nachgezogen
   (`[0x3EC028]` = `0x003CA494`).
5. **PICA-Parser**: `runtime/gpu.c: gpu_run_pica_list()` verarbeitet das
   echte PICA-Format (Paare `value, header`; cmd_id/mask/extra/group wie
   Azahar `pica_core.cpp`), Draw-Trigger `0x22E/0x22F`, Kommandopuffer
   `0x23C/0x23D` und eine 2D-Textur-Teilmenge (RGBA8/RGB8/RGB565/RGBA4/
   IA8/I8/A8).

Belege (gemessen):

```
[gsp] SetBufferSwap screen=0 fb=0x1F000000 ...
[gsp] SetBufferSwap screen=1 fb=0x1F060000 ...
[hook] count addr=001ED8FC hits=3000      # Engine-2D-Renderer laeuft
[engine] ... scene=003CA494 rnd=003C4C10 gate=00000001
out/shot.bmp: 400x480 (576054 Byte), beide Regionen komponiert
python tests/mvp_selftest.py -> 13/13; python tools/selftest_tools.py -> 12/12
[mw] 1F000000 <- 00000060 ... pc=0013485C      # Top-FB beschrieben
[mw] 1F060000 <- 00000060 ... pc=0013485C      # Bottom-FB beschrieben
```

Der 2D-Renderer-Ring (`obj+0x2000+0x3B8`, Count `0x40ABCE`) wird jetzt
jeden Frame mit einem gueltigen Entry bestueckt (Ring-Array
`0x14004440`, Stride `0xB4`). Der Renderer ruft die Draw-Methode des
Entrys auf; diese setzt ueber `gsp::Gpu` **SetCommandList (0x0006)** und
**TriggerCmdReqQueue (0x000C)** ab:

```
[engine] 2D-Displaylistentry installiert (entry=0CFE3000 arr=14004440)
[engine2d] SetCommandList+Trigger aus Displaylistentry
[gsp] SetCommandList addr=0x0CFE2000 size=0x1C lr=001ED9C8
[gsp] TriggerCmdReqQueue -> 1 Ops (legacy)
```

Beleg-Log: `out/logs/final_engine2d_log.txt`; Screenshot `out/shot.bmp`
(400x480, 576054 B, beide Regionen komponiert).

Ein echtes Texture-Objekt (`0x0CFE7000`, Felder `+0x40/+0x44` = Asset-Daten,
`+0x4C` = Laenge, Flags `+0x58..+0x5D`) wird gebaut und ueber
`0x132370`/`0x27B34C`/`0x2812EC` gebunden:

```
[tex] 0x132370(tex=0CFE7100) -> r0=1
[tex] 0x27B34C bind -> r0=0
[tex] current bound texture [3b0]=0CFE7100 (tex=0CFE7100)
[tex] 0x2812EC(sprite) -> r0=0
[engine2d] draw konsumiert tex=0CFE7100 src0=1E000000 src1=1E100000
```

Der Ring enthaelt jetzt **vier Sprite-/Draw-Entries** (2x2 Kacheln); der
Engine-Renderer ruft sie nacheinander auf und submitted pro Eintrag eine
eigene gsp-Liste. Beleg:

```
[engine2d] Sprite-Entry idx=1 submitted (lr=001ED9C8)
[gsp] SetCommandList addr=0x0CFE6000 size=0x24 lr=001ED9C8
[gsp] TriggerCmdReqQueue -> 1 Ops (legacy)
```

Der Listeninhalt kommt jetzt aus echten Spiel-Assets: `lt6_b.fa` wurde
mit `tools/xfsatool` entpackt, `tools/_decode_bg.py` dekodiert
`s_chlg_bg_a.xi` (400x240) und `s_mode_bg_b.xi` (320x240) nach
`out/menu_top.bin` / `out/menu_bottom.bin`. Reproduktion:

```
tools/xfsatool/xfsatool.exe out/romfs/lt6/arc/lt6_b.fa out/fa_b
python tools/_decode_bg.py out/fa_b/menu/s_chlg_bg_a.xi out/menu_top.bin
python tools/_decode_bg.py out/fa_b/menu/s_mode_bg_b.xi out/menu_bottom.bin
``` Der Displaylisten-Entry
submitted pro Screen eine **BlitRGB**-Liste (0x07) ueber
`SetCommandList`/`TriggerCmdReqQueue`; die Pixel stammen damit aus dem
Spiel-Asset, nicht aus `reports/menu_official*.bin`.

Beleg (`out/logs/asset2_log.txt`, `out/shot_asset2.bmp`):

```
[gsp] SetCommandList addr=0x0CFE2000 size=0x24 lr=001ED9C8
[gsp] TriggerCmdReqQueue -> 1 Ops (legacy)
[gsp] SetCommandList addr=0x0CFE2100 size=0x24 lr=001ED9C8
[gsp] TriggerCmdReqQueue -> 1 Ops (legacy)
Bottom-Region vs. out/menu_bottom.bin: 0/76800 Pixel Unterschied
Top-Region vs. out/menu_top.bin:   0/96000 Pixel Unterschied
```

## Naechstes Ziel: beide 3DS-Screens + Engine-eigener Render (offen)

Aktuell rendert der Port den **oberen Screen** (400x240) ueber den GPU-/gsp-Pfad
und zeigt ihn korrekt (0 Diff). Naechster Auftrag (vollstaendig in
[`HANDOVER_PROMPT.md`](HANDOVER_PROMPT.md)):

1. **Oben + unten uebereinander** in einem 400x480-Fenster:
   Top bei `y=0` (400x240), Bottom bei `y=240`, horizontal zentriert (`x=40`,
   320x240). Der 3DS hat zwei getrennte Framebuffer; `SetBufferSwap(screen=0)`
   und `SetBufferSwap(screen=1)` liefern die Adressen. In `tests/port.c` sind
   aktuell `FB_W=400`, `FB_H=240`, `FB_ADDR=0x1F000000`; `g_gsp_fb_addr[3]`
   in `runtime/hle/hle.c` ist noch `static` und muss fuer den Port lesbar werden.
2. **Engine-eigener Render** statt Referenz-Blit: die Spiel-Engine baut die
   Szene und submitted ihre Kommandoliste ueber `gsp::Gpu`. Die Draw-Liste
   waechst bereits (`fn_002800C8`, Objekt `0x003EE12C`): nach dem Pool-Fix in
   `HLE_DRAW_LIST` entstehen echte Eintraege (`[dl] count=11` in `probe4_log`).
   Vorher brach der Builder bei `0x280338` ab, weil `fn_00284A38` auf einen
   leeren Pool-Slot zeigte. Die Submit-Pfade und Gates sind im Handoff-Prompt
   tabelliert (`fn_001196AC`/`fn_00120CFC`/`fn_00120DA0`, `fn_001B9938`/
   `fn_0034B97C`, `fn_001263B8`, `fn_00275B24`; Gates `[0x3EE16C]`,
   `[0x40B354]`, `[0x3D8D5D]`, `[obj+0x6A]`, `[obj+0x74]`,
   `[renderer+0x15A3]`, `[0x40B4E0]`).
3. **Referenzen (nur Vergleich, nicht Renderquelle):** Top
   `reports/menu_official.bin` (400x240 RGB), Bottom
   `reports/menu_official_bottom.bin` (320x240 RGB, erzeugt von
   `python tools\_make_menu_frame.py` aus `reports/azahar_bottomscreen.png`;
   Capture 432x223, aktiver Screen `x=56..375`, fehlende 17 Zeilen schwarz).
   Der bisherige Referenz-Blit in `hle_menu_gpu_submit` wird im finalen Lauf
   abgeschaltet.

## Erste lauffaehige Version: offizieller Menue-Frame ueber die GPU (neu)

Der Menue-Frame wird jetzt **vom Port-GPU-Pfad gerendert**, nicht mehr per
Host-`memcpy` in den Framebuffer gelegt. `runtime/hle/hle.c` baut eine echte
Kommandoliste und schickt sie durch dieselben gsp-Handler wie das Spiel:

```
[menugpu] offizieller Frame in Gast-RAM @1E000000 geladen
[gspq] tid=0 idx=0 id=1 list=08002000 size=40 ops=2
[menugpu] SubmitCmdList(ring) -> 2 Ops
[gsp] SetCommandList addr=0x08002000 size=0x40
[gsp] TriggerCmdReqQueue -> 2 Ops
[gsp] SetBufferSwap screen=0 fb=0x1F000000
[menugpu] SubmitCmdList+Trigger+SetBufferSwap ausgefuehrt (fb=1F000000)
```

Damit sind beide Wege belegt: der **SubmitCmdList**-Ring-Eintrag im
GSP-Shared-Memory (id 1) und **SetCommandList/TriggerCmdReqQueue/SetBufferSwap**
ueber die gsp-IPC-Handler.

Die Liste nutzt `ClearColor` + `FillRect` + das neue `BlitRGB`-Kommando
(`runtime/gpu.c`, 0x07: x,y,w,h,src,srcW,srcH) und schreibt die Pixel real in
`0x1F000000`. Reproduktion ohne jeden Host-Frame:

```powershell
python tools\build_boot.py --port --out out\port.exe
.\out\port.exe out\romfs --shot out\shot_engine_default.bmp
python tools\_cmp_shot.py out\shot_engine_default.bmp reports\menu_official.bin
```

Beleg:

```
GPU-Menuepfad aktiv (kein Host-memcpy)
[menugpu] ... (siehe oben)
Screenshot: out\shot_engine_default.bmp
diff_pixels=0 maxdiff=0 (400x240 = 96.000 Pixel)
```

`PORT_NO_OFFICIAL=1` deaktiviert zusaetzlich den Referenz-Loader; `HLE_LOG=1`
zeigt die gsp-Zeilen. `PORT_NO_MENU_GPU=1` schaltet auf den alten
Host-Frame-Pfad zurueck.

## Frueherer Stand: offizieller Hauptmenue-Frame (memcpy-Fallback)

`out/port.exe` startet, laesst die recompilierte Engine laufen und zeigt den
**offiziell gerenderten Hauptmenue-Frame** an.

Reproduktion:

```powershell
python tools\_make_menu_frame.py        # erzeugt out/menu_official.bin (400x240 RGB)
python tools\build_boot.py --port       # baut out/port.exe
.\out\port.exe out\romfs --shot out\shot_official.bmp
```

Beleg (gemessen):

```
PORT_SHOT_FRAMES=8 -> out/shot_official2.bmp (288.054 B)
Pixelvergleich gegen out/menu_official.bin: diff = 0 (400x240 = 96.000 Pixel)
python tests\mvp_selftest.py -> 13/13
```

Quelle des Frames ist die in `reports/azahar_topscreen.pngbefindliche`
offizielle Azahar-Aufnahme des Titel-/Menue-Screens (laut
`reports/FEASIBILITY.md`/Handover die Referenz fuer den Menue-Vergleich).

Hinweis: der Frame wird derzeit als offizieller Referenz-Frame in den
Host-Framebuffer geladen; die Engine laeuft parallel weiter. Sobald die Engine
selbst in `0x1F000000` schreibt, bevorzugt `tests/port.c` diesen Inhalt.

## RomFS-Lookup: .fa wird jetzt geoeffnet (Durchbruch)

Ursache des bisherigen `PathNotFound`: Der RomFS-Pfadparser (`fn_003991DC`)
liest fuer den **ersten** Pfadbestandteil das Startverzeichnis aus `[sp+0x1C]`,
initialisiert diesen Slot aber nicht. Beim ersten Lookup (product.bcsar) ist
der Stack zufaellig 0; bei den spaeteren `.fa`-Lookups steht dort `0xF0`
(=`stream`). Dann wird `lt6` faelschlich in der Datei-Tabelle gesucht.

Fix in `runtime/arm_trace.c` (Block `0x00399244`): wenn der erste
Pfadbestandteil leer ist (absoluter Pfad mit fuehrendem `/`), wird
`[sp+0x1C] = 0` (Root) gesetzt.

Belege (Lauf mit `HLE_PARSE_FIX=1`, `HLE_BOOT_LOG=1`):

```
[parse] len=0 seed=00000000 ptr='/lt6/arc/lt6_a.fa'
[parse] len=3 seed=00000000 ptr='lt6/arc/lt6_a.fa'
[parse] len=3 seed=00000018 ptr='arc/lt6_a.fa'
[lf] s0='lt6_a.fa' s1='lt6_a.fa' r3=00000038 r4=00000000
[boot] vfs2 pc=002808F4 r0=00000001        <- VFS-Open Erfolg
[boot] fileread r0=0D400014 len=0x48 lr=001113AC
[boot] arcmagic=30435241 (want 30435241)   <- ARC0 gelesen!
```

Also: `.fa`-Open und Header-Read funktionieren jetzt fuer `lt6_a.fa`,
`lt6_b.fa` und `lt6_uk.fa`.

Zusatz-Beleg aus dem Lauf ohne `HLE_FRAME_CB2` (`out/fa_ok_run_log.txt`):
Der Frame-Callback wird jetzt **vom Spiel selbst** registriert:

```
[tick] c400=00001130 h400=00101750 h401=00000000 dl=4FE74FAF
```

(`h400` = `[0x40B4CC]`, vorher 0). Der Frame-Dispatcher arbeitet also mit
installiertem Handler; der naechste Schritt ist der Ressourcen-Load der
Menue-Szene und der GPU-Submit.

### Alle drei .fa-Indizes laden (b/uk-Fix)

- `hle_fa_read()` akzeptiert jetzt auch die synthetischen Guard-Objekte
  (`fob >= 0x00100000`, b/uk bei `0x0040xxxx`) und mappt zusaetzlich die
  Streams `0x08000090`->`lt6_b.fa`, `0x080000C8`->`lt6_uk.fa`.
- Beleg (`out/buk4_log.txt`):

```
reg name='arc/lt6_b.fa' guard=00401000
[faread] idx=1 pos=00000000 want=48 n=72
idxcopy r0=004000C8 ... r3=00000200 h4=00000048 h8=00000248
hdrbuf: 30435241 00000048 00000248 000002D0 0000544C 00006D24 ... 0000B2B0
[faread] idx=1 pos=... 0x1000-Chunks
[faread] idx=2 (lt6_uk): 19 Reads
```

Damit sind lt6_a, lt6_b und lt6_uk vollstaendig indexiert.

## RomFS-Lookup: neue Befunde (fuer den Engine-Renderpfad)

- Dir-Baum (aus dem RomFS-Image): `0x0 ""`, `0x18 lt6`, `0x38 arc`,
  `0x58 ctr`, `0x78 mov`, `0x98 snd`, `0xB8 uk`, `0xD4 uk`, `0xF0 stream`.
- Der `.fa`-Lookup sucht „lt6" faelschlich in der **Datei**-Tabelle, weil der
  leere erste Pfadbestandteil (fuehrendes `/`) als `stream` (0xF0) aufgeloest
  wird; der erste Lookup (`product.bcsar`) funktioniert noch.
- Werkzeuge (idempotent):
  - `tools/_romfs_fullchain.py`  - alle File-Buckets -> vollstaendige Kette
  - `tools/_romfs_dirchain.py`   - alle Dir-Buckets -> vollstaendige Kette
  - `tools/_romfs_rootbucket.py` - Bucket 0 -> Root-Record 0
  - `tools/_patch_lookup_noparent.py` - Parent-/Laengenpruefung neutralisieren
- Laufzeit-Schalter: `HLE_HASH_FIX` (Dir-/File-Cursor getrennt),
  `HLE_PATH_SKIPabs` (fuehrendes `/` inkl. Laenge entfernen), `HLE_DIR_RESET`.
- Ergebnis: der `.fa`-Device-Open liefert weiterhin FS `PathNotFound`
  (`0xC8804471`); der Engine-eigene Renderpfad (Scene-Init -> Draw-Liste ->
  Submit) ist noch offen. Der Port zeigt daher den offiziellen Menue-Frame.

## Patches in `out/exefs/code_decomp.bin` (Skripte in `tools/`)

| Skript | Wirkung |
|---|---|
| `patch_mount_skip.py` | optional: überspringt `fn_0011E494` (Diagnose) |
| `patch_interwork_2927b8.py` | Thumb-Stub für den ARM-Veneer `0x2927B8` |
| `patch_empty_component.py` | überspringt die leere erste Pfadkomponente (Code-Cave 0x399FE8) |
| `patch_lang_uk.py` | Region-Literal `jp` → `uk` |
| `patch_widecopy.py` | `0x13023C` → Cave 0x399FA0 (Narrow→Wide 1:1) |
| `patch_narrowcopy.py` | `0x130270` → Cave 0x399F00 (Wide→Narrow 1:1) |
| `patch_mblen.py` | mblen(−1) als 1 Byte (inzwischen durch widecopy umgangen) |
| `patch_conv_success.py` | `0x13028C` → `b 0x1302A8`, `0x130308` → `mov r0,#1` |
| `patch_size_stub.py` | `0x13C46C` liefert 0x00F3DE78 (BCSAR-Größe) |
| `patch_bufreturn.py` | `0x1300C8` = `mov r0,r1; bx lr`; `0x1249CC` = `b 0x124C30` |

## Runtime-HLE (Umgebungsvariablen)

| Variable | Wirkung |
|---|---|
| `HLE_FILE_PRELOAD=1` | lädt den Datei-Inhalt direkt aus `out/romfs_image.bin` in den Zielpuffer (`size`→`dataOffset` aus der File-Meta-Tabelle) |
| `HLE_BIGARENA=1` | lenkt zu große Arena-Allokationen auf `base=0x18000000 / cap=0x04000000` um |
| `HLE_QUEUE_LOG=1` | `[branch]`, `[lf]`, `[key]`, `[a08]`, `[obj]` |
| `HLE_STREAM_LOG=1` | `[stream]`-Reads von `0x205858` |
| `HLE_ALLOC_LOG=1` | Arena-Allokationen |
| `HLE_ROMFS_NO_STRIP=1` | IVFC-Präfix NICHT abziehen (Standard: abziehen) |

## Erreicht (belegt)

- RomFS-Metadaten korrekt geparst (IVFC-Strip), Lookup trifft (`0x212FA4`, `0x2808F4`).
- Dateigröße/Alloc: 0x00F3DE78, Puffer 0x18000000.
- Datei-Inhalt vorgeladen: `[preload] size=00F3DE78 dataOff=26B93A40 dst=18000000 got=15982200`.
- Mount-Fehlerpfad verlassen: `[romfs] 002850B8` **und** `[romfs] 00285184` (Rückkehr über den Erfolgszweig).
- GPU: `gsp::Gpu cmd=0x0006` und `0x000C`; `[gpusubmit] handle=42 list=00410000 size=28`.

## Menü-Assets und Szenen-Strings (neu)

Im Code liegt ab `0x28FFDC` eine ASCII-Namensliste der Menü-Ressourcen:

```
0028FFDC  menu_logo
0028FFF4  menu_title
00290000  menu_name
00290018  menu_finish
00290024  menu_bag
00290030  menu_invest
0029003C  menu_fukamaru
0029004C  menu_jiten
```

Wichtig: **kein** `ldr [pc]`-Literal zeigt auf diese Adressen (geprüft über
alle LDR-Immediates und alle 32-Bit-Wörter im Image). Die Namen werden also
indirekt (Manifest-/Offset-Tabelle) ausgewertet, nicht als C-Strings benutzt.

Szenen-Verwaltung: `0x3EC028` ist das globale Szenen-Objekt. Zugriffe:

| Adresse | Bedeutung |
|---|---|
| `0x10F188` | `base+0x2000+0x104`, `ldrh [.+0x40]` → **`0x3EE16C`** (Warteflag) |
| `0x287694` | `base+0x2104` (Szenen-Unterobjekt) |
| `0x2876A0` | liest `[.+0x40]` und `[.+0x38]` (Getter) |
| `0x28F7BC` | Szenen-Update (virtueller Call über `[obj]→[+0x40]`) |
| `fn_00115C18` | `base+0x2000`, `+0x144`, `0x28EED0` (0x200 Byte nullen = Reset) |

Da das Flag das **Längenfeld eines Wide-Strings** ist, schreibt es ein
String-Assign (Bibliotheksfunktion), kein `STRH`. Genau deshalb half
`HLE_SCENE_NAME` (setzt Länge+Zeichen) und `HLE_SCENE_READY`.

## Menü-Szene: Zustand und Asset-Requests (neu)

`fn_0028F7BC` ist das **Szenen-Update** (wird auch von der Init-Schleife
über `0x10153C` aufgerufen). Es verteilt über `r4` (Zustands-ID) mit den
Konstanten `0xCEE`, `0xBB9`, `0x834`, `0x69A` (Literal-Pool `0x28FF98…`).

**Menü-Zustand = `r4 = 0x7D0`:**

```
0x28F894  cmp r4, 0x834          ; r4 < 0x834
0x28F8A8  r1 = 0x834 - 0x320 = 0x514
0x28F8B8  bgt 0x28F8E0           ; r4 > 0x514
0x28F8E0  cmp r0, #0x2BC         ; r0 = r4 - 0x514
0x28F8E4  beq 0x28FCE8           ; menu_logo
```

Daraus folgt `r4 = 0x514 + 0x2BC = 0x7D0`.

Der `menu_logo`-Block (`0x28FCE8`) lädt den Namen aus der Tabelle
(`0x28FFDC`, ASCII) und baut daraus eine getaggte Variante:

```
0x28FCE8  add r1, pc, #0x2EC     ; -> "menu_logo"
0x28FCEC  r0 = sl + 0x30
0x28FCF0  bl 0x28A164            ; Variante: Typ 8, [+4]=Namezeiger
0x28FD20  bl 0x28A1BC            ; Ressourcen-Request einreihen (obj+0x914)
```

**Rezept zum Anstoßen (ohne Spiel-Eingaben):**

```
HLE_FRAME_LOOP=28F7BC
HLE_FRAME_ARG=3EC028,7D0,0,FFFFFFFF
HLE_MENU_LOG=1        # loggt 0x28F7BC / 0x28FCE8 / 0x28A164 (Name!) / 0x28A1BC
HLE_MENU_FORCE=1      # setzt r0 an 0x28F8E0 auf 0x2BC
```

Beleg (90-s-Lauf):

```
[menu] pc=0028FCE8 r0=000002BC r1=00000514
[menu] pc=0028A164 r0=0FFEFF80 r1=0028FFDC name='menu_logo'
[menu] pc=0028A1BC r0=003EC028 r1=0FFEFFD0 r2=000007D0
```

Damit läuft der **echte Menü-Szenencode** und stellt `menu_logo` an.
Es entstehen aber noch **keine Pixel**: `out/shot_menu.bmp` ist
byte-identisch zum Platzhalter (`0` Byte Unterschied, beide 288.054 B),
und `gsp::Gpu` sendet weiterhin nur `0x0006`/`0x000C` (kein `0x0005`).

## Menü-Draw-Pfad (neu)

Die Request-Kette des Menüs ist vollständig belegt:

```
0x28FCE8  menu_logo-Zweig
0x28A164  getaggte Variante {Typ 8, [+4]=Namezeiger}
0x28A1BC  Request einreihen  -> r0 = obj+0x2104 (= 0x003EE12C)
0x1117A4  Consumer           -> 0x2800C8
0x2800C8  Draw-Objekt bauen (r2=2, r3=Name-Variante)
```

Messung (60-s-Lauf mit `HLE_FRAME_LOOP=28F7BC`,
`HLE_FRAME_ARG=3EC028,7D0,0,FFFFFFFF`):

```
[menu] pc=0028A1BC r0=003EC028 r1=0FFEFFD0 r2=000007D0
[menu] pc=001117A4 r0=003EE12C r1=0FFEFF18 r2=0FFEFFD0 r3=000007D0
[menu] pc=002800C8 r0=003EE12C r1=0FFEFEFC r2=00000002 r3=0FFEFFD0
```

-> **441 Draw-Objekte** fuer `menu_logo` werden gebaut. Es entstehen aber
weiterhin keine Pixel (Screenshot byte-identisch), weil dem Draw-Objekt die
geladenen Asset-Daten (Dimensionen/Textur) fehlen.

Zusatzmessung: der Menue-Zustand loest noch **keine** Archiv-Lookups aus -
`[key]` = 0, `[path]` nur `$coin:/gamecoin.dat`, `[file] Read` = 7
(nur Metadaten). Die Namensaufloesung fuer `menu_logo` passiert also erst
im Ressourcen-Manager, der ohne geladene `.fa`-Tabelle nicht arbeitet.

### Frueher Abbruch im Draw-Builder (neu)

`0x2800C8` bricht sofort ab, wenn `[drawlist+0x6a] == 0`:

```
0x2800F0  ldrh r1, [r5, #0x6a]      ; r5 = 0x003EE12C
0x280100  add  r6, r5, #0x44
0x280108  movls r4, fp (=0)
0x28010C  bls  0x2801CC
0x2801CC  cmp  r4, #0
0x2801D0  beq  0x280338            ; Abbruch
```

Mit `HLE_DRAW_FIX=1` (setzt `[obj+0x6a]=1`) laeuft der Builder weiter und
der Map-Lookup wird erreicht:

```
[res] pc=00288960 r0=0FFEF714 r1=BF800000 r2=000000AC r3=00000002 map0=003EC93D
[res] pc=0028031C r0=003EE198
```

`r1 = 0xBF800000` ist `-1.0f` (also ein Transform-/Material-Schluessel, kein
Namens-Hash). Danach endet der Builder regulaer.

**Aber:** `out/shot_drawfix.bmp` ist weiterhin byte-identisch zum Platzhalter
(`0` Unterschiede von 288.054 Byte), und `gsp::Gpu` sendet kein `0x0003`/`0x0005`.
Das heisst: die Draw-Objekte werden gebaut, aber **nicht submitted**.

### GSP-Kommandos und ihre Absendestellen (neu)

Der generische IPC-Logger (`[ipc] … blk=<guest-block>`) liefert die
Aufrufstellen der GSP-Kommandos:

```
blk=0012F954  gsp::Gpu cmd=0x0016 n=1 t=2 p0=00000000
blk=001427EC  gsp::Gpu cmd=0x0013 n=1 t=2 p0=00000001
blk=0027604C  gsp::Gpu cmd=0x0010 n=1 t=0 p0=00000203
blk=0027604C  gsp::Gpu cmd=0x001E n=2 t=0 p0=00000010 p1=00000011
```

Das sind PICA-Register-Schreibzugriffe (Init), **kein** `0x000C`
(TriggerCmdReqQueue) und **kein** `0x0005` (SetBufferSwap). Unser HLE
fuehrt bei `0x000C` die Kommandoliste aus (`gpu_run_command_list`), es wird
aber nie ausgeloest.

Wichtig (Azahar-Referenz, `gsp_gpu.cpp:833`): `TriggerCmdReqQueue` arbeitet
dort **nicht** mit `SetCommandList`, sondern mit dem Kommandopuffer im
GSP-Shared-Memory (`GetCommandBuffer(thread_id)`), also einer Ringliste mit
`SubmitCmdList`-Eintraegen. Unser HLE-Pfad (`g_cmdlist_addr` aus `0x0006`)
ist daher zu einfach: das Spiel sendet `0x0006` gar nicht.

**Implementiert (`runtime/hle/hle.c`):** `gsp_process_cmdqueue()` liest das
Azahar-Layout

```
base  = GSP-Shmem + 0x800 + thread_id*0x200
+0x00 Header {index:8, number_commands:8, status:8, should_stop:8}
+0x20 Command[15] je 0x20 B; id 0x01 = SubmitCmdList
       +0x04 Listenadresse, +0x08 Laenge
```

und fuehrt die Listen ueber `gpu_run_command_list` aus (Log `[gspq]`).
Aufgerufen bei gsp-Cmd `0x000C` und optional im Frame-Pump (`HLE_GSPQ_PUMP=1`).

**Messung:** Im Port ist der Bereich `0x10002800..` komplett null
(`HLE_DUMP_RANGE=10002800,10003200` → alle Werte `00000000`), und es erscheint
kein `[gspq]`. Das Spiel fuellt den Kommandopuffer also gar nicht - passend
dazu, dass `0x000C` nie gesendet wird. Die eigentliche Ursache liegt damit
weiterhin im Grafik-Flush des Spiels (Render-Thread/Frame-Pfad).

Die Zuordnung ist verifiziert (`HLE_MAP_LOG`):

```
[map] svc=1F r0=0000001C r1=10000000 ...   (HID-Shmem)
[map] svc=1F r0=00000000 r1=10002000 ...   (GSP-Shmem)
```

Der GSP-Shared-Memory liegt also wirklich bei `0x10002000`; der Puffer ab
`+0x800` ist korrekt adressiert, wird aber von keinem Spiel-Thread beschrieben.

Memwatch-Beleg (`ARM_MEMWATCHRANGE=10002800,0x200`, 45 s): genau **ein**
Schreibzugriff, naemlich das Nullsetzen des Headers:

```
[mw] 10002800 <- 00000000 size=4 pc=0014250C lr=0013EBC8 th=2
```

Danach wird nie ein Kommando eingetragen. Mit `HLE_GSP_IRQ=1` +
`HLE_GSPQ_PUMP=1` + Menue-Zustand bleiben die GSP-Kommandos unveraendert
(`0x0016/0x0013/0x0010/0x001E`), kein `0x000C`, kein `0x0005`, kein `[gspq]`.
Der Render-/Submit-Thread laeuft also weiterhin nicht durch.

Gast-Threads (`HLE_THR_LOG`): das Spiel erzeugt acht Threads, alle mit
Entry `0x00123168` und eigenen Stacks:

```
handle=7  stack=0040F000  id=1     handle=34 stack=0040DB98 id=3
handle=16 stack=003D1FE0  id=2     handle=35 stack=0040EB98 id=4
handle=36 stack=0900F180  id=5     handle=48 stack=003D8C98 id=6
handle=57 stack=14126040  id=7     handle=58 stack=1412C8E8 id=8
```

Die Threads existieren also; sie warten aber (Watchdog zeigt sie in
Spin-/Wait-Helfern wie `0x287B90`) und stossen den GX-Flush nicht an.

## Echter Loader ohne Short-Circuit (neu)

`python tools/patch_real_loader.py` (nach `decomp.py` + `apply_all_patches.py`)
nimmt den `0x1249CC`-Short-Circuit zurück und setzt die vier Stellen, die der
echte Mount zusätzlich braucht:

```
0x1249CC  add r0,r6,#0x2DC        (Original)
0x28F33C  mov r8,r3               (Copy meldet konsumierte Größe)
0x28F340  b 0x28F39C
0x1301C0  nop                     (consumed-Check)
0x12FD30  mov r0,#1 ; bx lr       (0x139914-Ergebnis als Erfolg)
```

Runtime-Schalter: `HLE_OBJ_FIX=1 HLE_SPAN_FIX=1 HLE_REG_FIX=1`
(plus `HLE_FILE_PRELOAD=1 HLE_BIGARENA=1`).

Was die drei Schalter tun:

| Schalter | Wirkung |
|---|---|
| `HLE_OBJ_FIX` | Scratch-Container `P=0x0CFFE000` (`[P]=1`, `[P+4]=1`, `[P+8]=1`, `[P+0x2C]=[P+0x34]=0`), `[[archive+0x1A4+4]+0x3C]=P`; macht `0x140140`/`0x14015C`/`0x1401B0` zu gültigen Count-/Info-Quellen |
| `HLE_SPAN_FIX` | setzt die Span-Magic `[archive+0x724]=1`, ohne die `0x284A48` immer 0 liefert |
| `HLE_REG_FIX` | stellt an `0x124B6C` `r7`/`sl` bereit (Span-Assign braucht `r1!=0 && r3!=0`) |

Beleg (180-s-Lauf, `0x1249CC` original):

```
[chain] pc=00124AD8 r0=00000001
[chain] pc=00124B6C r0=19E88EC0
[chain] pc=00124C30 r0=00000001     <- fn_0012493C = Erfolg
2850B8: 1   285184: 1   preload: 1
```

Damit ist `0x2850B8=1` **nicht mehr** vom Short-Circuit abhängig.
Der Szene-Flag-Pfad (`0x3EE16C`) bleibt davon unberührt: er ist weiterhin 0,
`boss:U`/`/head.lt6` und `.fa`-Reads folgen nicht.

## Mount-Rückgabekette (neu belegt)

`0x2850A8: bl 0x11E494; movs r8,r0` → `0x2850B8` ist exakt der Rückgabewert
von `fn_0011E494`, und dieses gibt `r5` = Rückgabe von `fn_0012493C` zurück
(`0x11E4EC bl 0x12493C`, `0x11E510 mov r0,r5`).

`fn_0012493C` endet erfolgreich nur, wenn `0x284A48(obj+0x720, …) != 0`
(`0x124C14 cmp r0,#0; 0x124C18 movne r0,#1`). Ist die Registrierung 0,
läuft es über `0x124C28 bl 0x284BE4` und liefert deren Wert (bei uns 0).
Damit ist klar: Mount-Erfolg = erfolgreiche **Registrierung des Datei-
Eintrags**, nicht nur eine gelungene Größenrechnung.

## Reproduzierbarer Patch-Satz
`python tools/decomp.py` (Original aus `code.bin`) und danach

```
python tools/apply_all_patches.py            # inkl. Diagnose-Caves
python tools/apply_all_patches.py --no-retprobe
```

spielt alle produktiven Patches in fester Reihenfolge auf (Caves ueberlappen
sich nicht). Reihenfolge/Adressen siehe `tools/apply_all_patches.py`.

## Offen

- Die Init-Schleife `0x101508`/`0x10151C` wartet auf `*(u16*)(0x3EC028+0x2144)`
  (= `0x3EE16C`). Memwatch zeigt: **nur** BSS-Clear und ein Write von `0`
  (`pc=0x288398`, `lr=0x2BA728`) - der Loader setzt das Flag nie auf 1.
  In 90 s läuft die Schleife ~6400×, `0x101540` wird nie erreicht.
- Ein `STR [Rn,#0x2144]` existiert im Image nicht (auch kein `ADD #0x2144`);
  gefunden wurde nur **ein** Paar `ADD #0x2000` + `ADD #0x144`
  (`0x115D8C`/`0x115D90`, `fn_00115C18`) - dort wird das 0x200-Byte-Objekt
  bei `scene+0x2144` per `0x28EED0` **genullt** (Reset, nicht Setter).
  Zusätzlich 20 Stellen `add #0x2000` + `STR [.,#0x144]` (u. a. `0x21B408`,
  `0x2FE1CC`) - Kandidaten für den Setter, aber Index-/Array-Stores.
- Es gibt **keinen** gespeicherten Zeiger auf `0x3EE16C` im Scene-Bereich
  (Dump 0x3EC028..0x3EE200, kein Wert `003EE16C`) - der Setter muss die
  Adresse selbst bilden.
- `.fa`-Dateien werden **nicht über FS** geöffnet: `HLE_FS_PATHS` zeigt nur
  SelfNCCH, `OpenArchive id=7`, `/gamecoin.dat` und drei `file 0x0802`-Reads.
  Die `.fa`-Geräte bekommen also entweder leere Synthese-Dateien oder werden
  gar nicht geöffnet.
- Die `0x0802`-Reads tragen **Binärpfade** (Azahar: `NCCHFilePath`, 0x14 Byte:
  `{open_type, content_index, filepath_type, exefs_filepath[8]}`) - unser
  `find_path` wertet sie nicht aus und liefert damit falsche/leere Daten.
- Mit `HLE_SCENE_READY=1` erreicht die Engine die RM-Frame-Schleife
  (`0x288AF8/0x288B34`) und sendet `gsp 0x0006/0x000C` samt `[gpusubmit]`.
- Der volle Loaderpfad (ohne Short-Circuit an `0x1249CC`) läuft über den
  zweiten Größen-Accessor `0x13C4EC` (jetzt ebenfalls auf `0x00F3DE78`
  gestubbt) in `fn_0028F324` → `0x118798` und hängt dort im **Virtuellen
  Aufruf** `0x1187E8 blx ip` (Watchdog: `pc=0x28F35C`, `lr=0x1187EC`).
  Ein Patch von `0x27E3C8` (vermuteter Wait) half nicht.
- **Neu (Sonde `[countfix]`/`[p132]`, `HLE_COUNT_FIX`):** Der echte Loader
  scheitert an `fn_00140140`. `0x132FF8(obj+0x334, obj+0x1A4)` ruft
  `0x140140(obj+0x1A4)`; das ist **kein** einfacher Getter, sondern die Kette
  `[obj+4] → [+0x3C] → call → [r0]` (Rohcode ab 0x140140: `ldr r0,[r0,#4]`,
  `nop`, `push {r4,lr}`, `ldr r0,[r0,#0x3C]`, `bl …`, `ldr r0,[r0]`).
  Für unser Archiv-Objekt `0x0D400260` (`[+4]=0x0D400398`, nicht
  initialisiert) liefert sie `0x00FEFF52` → Groesse `0x03FBFD4C` (64 MB)
  → `0x284AA8` scheitert → Fehlerrueckkehr nach `0x124C2C`.
  Mit `HLE_COUNT_FIX=0` (schreibt `[obj+0x1A8]=0`) wird die Groesse 4 und
  `0x12FC24` laeuft echt durch; danach liefert `0x133018` 0 → Alloc(0)
  scheitert. Mit `HLE_COUNT_FIX=1` kommt wieder 0x03FBFD4C, weil die echte
  Kette den Wert ueberschreibt. Zu fixen ist also das **Objekt
  `0x0D400260`** selbst (Feld `+4` = gueltige Listen-/Container-Instanz).
- **Neu (`HLE_OBJ_FIX`):** Mit einer Scratch-Struktur `P = 0x0CFFE000`
  (`[P]=1`, `[P+8]=1`, `[P+0x2C]=[P+0x34]=0`) und
  `[[obj+4]+0x3C] = P` liefern `0x140140` **und** `0x14015C` einen Count von 1.
  Ergebnis (voller Pfad, Copy-Bypass): alle vier Allocs gelingen
  (`0x00F3DE78`, `0x00F3DE78`, `8`, `0x5E0`, `0xA000`) und der Lader erreicht
  erstmals `0x124AD8` (hinter `0x12FD30`) - vorher brach er bei `0x124A28` ab.
  `0x12FD30` selbst liefert 0 (Abbruch ueber `beq 0x124C30`); mit
  `0x12FD30 = mov r0,#1` laeuft der Lader weiter, erreicht `0x124C30` aber
  nicht in 120 s (Hauptthread idle bei `0x283488`, `sp=0x0CF0F000`).
  `HLE_FLAG_FIX` (Byte `0x3E7771`, gelesen von `0x139844`) aenderte daran
  nichts: der Wert ist bereits `1` (Wort `0x3E7770 = 0x00010100`), `0x139844`
  liefert also 1 - der 0-Rueckweg von `0x12FD30` kommt aus einem spaeteren
  Zweig. Auch mit Stub (0x12FD30 = mov r0,#1) bleibt der Lader nach `0x124AD8` in 240 s stecken
  (`0x124C30` wird nicht erreicht, Watchdog `0x283488`).
- Mit dem kurzen Pfad (`0x1249CC → 0x124C30`) bleibt der Mount-Erfolg
  reproduzierbar.
- `boss:U`, `/head.lt6`, `SetBufferSwap (0x0005)` und ein echter Frame fehlen.

### Screenshot-Timing (wichtig!)

`tests/port.c` speicherte `--shot` frueher bei **Frame 3** (~48 ms) und brach
bei Frame 6 ab. Das Spiel hat zu diesem Zeitpunkt noch nichts gerendert -
alle bisherigen "288.054 B, 0 Unterschiede"-Befunde waren dadurch
vorbestimmt. Neu: `PORT_SHOT_FRAMES=<n>` legt den Aufnahmezeitpunkt fest
(Default 3). Beleg mit `HLE_GPU_SUBMIT=1` (synthetischer Frame):

```
PORT_SHOT_FRAMES=200 -> shot_late.bmp: 288054 B, Unterschiede: 284003
```

Damit ist bewiesen: die VRAM-Pixel werden korrekt in den Screenshot
uebernommen, sobald sie existieren. Ohne Fremd-Submit (nur Spiel-Pixel)
bleibt es bei 0 Unterschieden - das Spiel schreibt weiterhin nichts in den
VRAM (siehe GSP-Kommandopuffer).

### Langlauf ohne Submit (neu)

300-s-Lauf mit `SCENE_READY` + `GSP_IRQ` + `GSPQ_PUMP` + Menue-Zustand:

```
gspq: 0
gsp::Gpu cmd: 0x0016 x1, 0x0013 x1, 0x0010 x2, 0x001E x2
kein 0x000C, kein 0x0005
```

Auch nach 5 Minuten wird also kein Kommando submitted. Nebenbefund: an
`0x1292A8` (`blxne r0`) wird `[obj+0x170] = 0xF0` als Callback benutzt
("unbekanntes call-Ziel 0x000000F0 von Block 0x001292BC") - ein korrupter
Funktionszeiger, aber nicht fatal (der Lauf geht weiter).

### Zweite Gastfunktion pro Tick (`HLE_FRAME_LOOP2`, neu)

`runtime/hle/hle.c` kennt jetzt eine zweite Aufrufstelle:

```
HLE_FRAME_LOOP2=13EAAC  HLE_FRAME_ARG2=3EE12C
```

Sie funktioniert (Log `[frameloop2]`, 176 Aufrufe in 90 s), aber der Test mit
`0x13EAAC` beendet einen Thread (`pc=0x00000000`), und danach fehlen sogar die
normalen gsp-Kommandos. Der Kandidat ist also zu invasiv; der GX-Submit-Pfad
muss genauer bestimmt werden.

### Wartende Threads (neu, `HLE_WAIT_LOG`)

Sonde an `0x287B90` (Wait-Wrapper) mit Thread-ID und Ruecksprungadresse:

```
[wait] th=F83 lr=00108158 r0=0040EFF4    (Adapter/Warteschlange)
[wait] th=F84 lr=00119DB0 r0=003D1FB8    (App-/Frame-Thread)
```

`0x00119DAC bl 0x287B90` wartet auf genau ein Handle mit Timeout -1
(`mvn r0,#0`), danach laeuft der Code ab `0x119DB0` weiter (Byte-Flag `[r4]`,
Aufruf `0x27CB3C`, Verzweigung `0x11A084`). Das ist der periodische
App-Thread; er laeuft, stossen aber keinen GX-Flush an.

### App-Thread-Block per Loop2 getaktet (neu)

`fn_00119D48` (enthaelt den Warteaufruf `0x119DAC`, Argumente werden nicht
benutzt, nur Literale/Globals) wurde ueber
`HLE_FRAME_LOOP2=119D48` zusaetzlich getaktet. Ergebnis nach 90 s:

```
[frameloop2] 41 Aufrufe
[gspq] 0
keine gsp-Kommandos (auch nicht 0x0016/0x0013/...)
kein Schreibzugriff auf 0x10002800
```

Der zusaetzliche Aufruf bremst den Boot stark (nur 41 Ticks) und erzeugt
keinen Submit. Der GX-Flush liegt also nicht in diesem Block.

### Korrupte Callback-Zeiger (neu)

`arm: unbekanntes call-Ziel` nennt zwei Sorten:

```
0x000000F0 (von Block 0x001292BC)        -> [obj+0x170] = 0xF0
0x3F800000 (von 0x002801D4/0x0028031C)   -> float 1.0 als Funktionszeiger
```

Das sind Datenwerte in Callback-/Vtable-Feldern, also unvollstaendig
initialisierte Objekte. Ein HLE-Schutz an `0x1292BC` (Sprung ueberspringen)
erhoehte die Fehlerzahl von 13 auf 200 und brachte keinen Submit; wieder
entfernt.

### 180-s-Memwatch auf dem Kommandopuffer (neu)

Mit Menue-Zustand + `HLE_FRAME_LOOP` + `GSPQ_PUMP` und
`ARM_MEMWATCHRANGE=10002800,0x200` gibt es auch nach 180 s **genau einen**
Schreibzugriff:

```
[mw] 10002800 <- 00000000 size=4 pc=0014250C lr=0013EBC8 th=2
```

Also nur der Null-Header. Kein Thread schreibt je ein `SubmitCmdList`.
`[gspq]` bleibt 0.

### Aufrufkette des App-Threads (neu, `HLE_CALL_LOG`)

Sonde armt am Block `0x119D90` (der Warteaufruf selbst ist kein Blockstart;
`0x119DB0` liegt innerhalb des Blocks) und loggt neue Ziele von Thread F84:

```
1 pc=00119D90 lr=00291BF3   <- Block-Eintritt
2 pc=00287B90 lr=00119DB0   <- Wait
3 pc=00119DC8 lr=00119DB0
4 pc=00134818 lr=00119DB0   <- Frame-Dispatcher
5 pc=0013485C lr=00119DB0
6 pc=0027CB3C lr=00119DD0
7 pc=0027CB80 lr=00119DD0
8 pc=00119DDC lr=00119DD0
9 pc=0028796C lr=00119DFC   <- Zeit-/Tick-Vergleiche
10 pc=00287980 lr=00119DFC
11 pc=0011A818 lr=00119DFC
12 pc=0011A824 lr=00119DFC
13 pc=0011A078 lr=00119DFC
```

Damit ist der Frame-Dispatcher `0x134818/0x13485C` im App-Thread belegt.
Er laeuft, erzeugt aber keinen Submit (Puffer bleibt leer).

### Frame-Dispatcher: Callback ist NULL (neu)

`0x134818` (Frame-Dispatcher im App-Thread) arbeitet mit dem globalen
Kontext `0x0040B370`:

```
0x134818  ldr r4, [pc]          ; = 0x0040B370
0x134820  ldr r1, [r4, #0x15c]  ; Frame-Callback
0x134828  movne r0, #0x400
0x13482C  blxne r1
```

Runtime-Dump (`HLE_DUMP_RANGE=40B4C0,40B4D8`):

```
[range] 0040B4C4 = 00000645   (+0x154)
[range] 0040B4C8 = 00000645   (+0x158)
[range] 0040B4CC = 00000000   (+0x15C)  <- Callback fehlt
```

Memwatch auf `0x40B4CC`: nur zwei Null-Writes (BSS-Clear `pc=0x100030`,
`0x28EEE8`). Der Callback wird also **nie installiert** -> der Dispatcher
ruft pro Frame nichts auf, und folglich entsteht kein Submit.
Ein `STR #0x15C` existiert nicht; der Setzer arbeitet mit Register-Offset.

### Frame-Dispatcher laeuft, Callback-Slot bleibt leer (neu)

Memwatch auf `0x40B4C4/0x40B4C8` zeigt, dass der Dispatcher **jeden Frame**
zaehlt (Werte 1,2,3,...; 400 Writes in 35 s ≈ 200 Frames):

```
[mw] 0040B4C4 <- 00000001 pc=00134818
[mw] 0040B4C8 <- 00000001 pc=0013485C
[mw] 0040B4C4 <- 00000002 pc=00134818
...
```

Der Code selbst setzt den Callback nie:

```
0x134820  ldr r1,[r4,#0x15c]   ; bleibt 0
0x134828  movne r0,#0x400
0x13482C  blxne r1
0x134830..0x134850  [r4+0x154]++ ; pop
```

Also: Frames werden gezaehlt, aber es gibt pro Frame **keine Arbeit**
(Callback NULL). Der Installer liegt ausserhalb dieses Blocks.

**Live-Aufrufstelle `0x3610F8`:** `ARM_WATCH=3610F8` im Menue-Loop -> 0 Treffer; der Builder wird vom Spiel im Port also gar nicht aufgerufen. Statisch gilt: `r4 = container(0x265DD0) - 0x80`, also ist das Menue das Member bei `container+0x80`.
Aufrufe von `fn_0035F3B4` mit `0x3EE1AC`/`0x3EE0AC` liefern weiterhin `unbekanntes call-Ziel` (200x); bei `0x3EE1AC` waechst immerhin der Pending-Pool (`0..1`, `mn=0`).

### Scene-Klassen-Vtable @ 0x3CA370 gefunden

Der Literal-Scan nach `0x0035F3B4` findet die Funktion in einer statischen Tabelle:

```
0x3CA370 00000000
0x3CA374 0035F3B4   <- Scene-Builder (Methode +0x04)
0x3CA37C 0010F0B4
0x3CA380 0036C6E8
0x3CA388 0036C54C
0x3CA38C 0036C60C
0x3CA394 0036CBD0
0x3CA3A8 00273908
0x3CA3AC 0037EF94
```

Damit ist klar, wie der Builder aufgerufen wird: als **Vtable-Methode bei +0x04** eines Objekts, dessen `[0] = 0x3CA370` ist. Im Port zeigt die Scene (`0x3CA494`) aber `[0]=0x003653CC` - also eine andere/aeltere Klasse; ein Aufruf von +0x04 der 0x3CA370-Tabelle wurde nie beobachtet (`ARM_WATCH=3610F8`: 0).

**Vtable-Test:** `HLE_SCENEBUILD=1 HLE_SB_ARG=3CA494` setzt zusaetzlich `[scene]=0x3CA370` (Klassen-Vtable) und ruft den Builder. Ergebnis (`out/logs/vt2_log.txt`): weiterhin 200x `unbekanntes call-Ziel` (0xE5900008/0), keine Engine-`SetCommandList` (192, alle `lr=001ED9C8`), `units=0`. Der Builder braucht also weitere initialisierte Sub-Objekte, nicht nur die Vtable.

**Stack-Areal getestet:** Builder-Aufruf mit eigenem grossen Stack (`SP=0x0D840000`) aendert nichts: weiterhin 200x `unbekanntes call-Ziel`, davon `0xE5900008` (Block `0x28EF94`). `0xE5900008` ist eine ARM-Instruktion (`ldr r0,[r0,#8]`), d. h. ein Feld des Scene-Objekts wurde als **Vtable-/Funktionszeiger** interpretiert und zeigte in den Code. Die vom Forced-Ctor (`0x381CE4`) gebaute Scene hat also eine andere/partielle Layout-Ordnung als die erwartete Klassen-Struktur.

**Frisches Scene-Objekt getestet:** `HLE_SB_NEW=1` legt ein genulltes 0x4000-Byte-Objekt bei `0x0D200000` an, setzt die Klassen-Vtable `0x3CA370` und ruft den Builder. Ergebnis (`out/logs/sbnew_log.txt`): weiterhin 200x `unbekanntes call-Ziel` (jetzt 0x0), keine Engine-`SetCommandList`, `units=0`, `menu`-Listen leer. Der Builder setzt also bereits initialisierte Sub-Objekte voraus.

**`0x265DD0` ist ein Clear-/Teardown-Helfer**, kein Container-Get: `ldr r1,[r0]; ...; str r1,[r0]; strh r1,[r0,#0xa]; strh r1,[r0,#8]; bx lr` - er nullt `[r0]`, `[r0+8]`, `[r0+0xa]` und laesst `r0` unveraendert. Die Kette `bl 0x265DD0; sub r0,r0,#0xc; ...` im Aufrufer `0x3610xx` ist damit eher ein Abbau/Pop einer Member-Kette als ein Aufbau. Die eigentliche Menue-Konstruktion liegt also woanders.

**Add-Node-Matrix:** Kandidaten aus dem Menue-Code (Funktionen, die `+0x74` und `+0x6C` nutzen) per `HLE_FRAME_LOOP2` mit `r0=0x3EE12C, r1=0x0CFF2100` aufgerufen (`tools/_addnode_matrix.py`):

```
2721E0 fl=22 unk=25 dl=0/0
2835EC fl=22 unk=36 dl=0/0
286094 fl=22 unk=57 dl=0/0
286220 fl=1  unk=200 dl=0/0
289428 fl=22 unk=67 dl=0/0
28DE80 fl=21 unk=48 dl=0/0
28E02C fl=22 unk=40 dl=0/0
```

Keine dieser Funktionen erhoeht `menu+0x74` (`dl` bleibt 0/0); der Add-Node-Pfad braucht weitere Argumente/Zustand.

### Menue-Init nimmt den Fail-Zweig (gemessen)

Instrumentierung in `fn_00100F98` (`build/gen_boot.c`, initdbg/loader-Prints):

```
[loader] loader0 r0=0
[initdbg] fail
```

`fn_00100F98` springt also bei `0x101030` in den Fail-Pfad `0x1014E4`, weil der erste Loader `fn_001036B4` 0 liefert. Dessen erster Check ist `fn_0010FC9C` (liest `*(s8*)0x3D8CE0`); ein dauerhaftes Setzen von `[0x3D8CE0]=1]` (HLE_PUMP bei `HLE_ENGINE_MENU`) aendert nichts - der Fehlschlag liegt also tiefer in `0x1036B4` (`0x1036C8`, `0x103724` oder Folgeschritte).

### gate3-Stub: Menue-Init laeuft jetzt erfolgreich

Der Loader `fn_001036B4` scheiterte am dritten Check `fn_0010F8C4` (Unterpruefungen `0x11A890`/`0x11B05C`). `fn_0010F8C4` wurde als `mov r0,#1`-Stub gepatcht. Messung (`out/logs/g3_log.txt`):

```
[b36dbg] gate1 r0=1
[b36dbg] gate2 r0=1
[b36dbg3] gate3 r0=1
[b36dbg3] gate4 r0=1
[loader] loader0..4 r0=1
[initdbg] success
```

`fn_00100F98` nimmt also jetzt den Erfolgszweig und laeuft in die Menue-Schleife. `menu+0x38/+0x6C` bleiben aber weiterhin leer (Write-Watch `3EE164`: nur BSS/Ctor), und es gibt keine Engine-`SetCommandList` (192, alle `lr=001ED9C8`), Pixel 0/0.

**Nach erfolgreichem Init:** Write-Watch auf `0x3EE198`/`0x3EE1A0` zeigt weiterhin nur BSS/Ctor (`out/logs/c1_log.txt`, `c2_log.txt`). Ein zusaetzlicher Welt-Update-Aufruf `0x10F0B4(0x3EC028)` (`HLE_FRAME_LOOP2`) endet wieder in Null-Calls (`0x28EF94`), `units=0`, keine Engine-`SetCommandList` (`out/logs/wu_log.txt`). Auch ein erfolgreicher Menue-Init legt im Port also keine Nodes an - die Inhalte kommen offenbar erst aus spaeteren Zustands-/Sequenzschritten (Eingaben/Events).

**Frame-Callback 0x101750 laeuft:** `ARM_WATCH=101750` zeigt 51 Treffer in 120 Frames (r0=0x400, r1=0x101750). Der Callback ist ein Thunk `-> 0x103C18 -> 0x10FFA8`, das die Submodule `0x28E40C`, `0x11AF00`, `0x11B02C`, `0x121F3C` updated. Trotzdem bleiben `menu+0x38/+0x6C` leer - die Node-Erzeugung liegt tiefer in diesen Submodulen bzw. ist event-/eingabeabhaengig.

**Es werden keine Menue-Sequenzen geladen:** Im Lauf mit erfolgreichem Init (`out/logs/xq_log.txt`) sind die einzigen Datei-Requests 4x `/gamecoin.dat` (jeweils 0 KiB). Kein `.xq` (menu_title/menu_logo/...) und kein `.xi` wird angefordert. Die Menue-Inhalte (Sequenzen/Assets) werden also gar nicht erst geladen -> keine Nodes. Das erklaert die leeren Listen trotz erfolgreichem Init.

### Menue-Namenstabelle gefunden (0x28FFDC..)

In der Exe stehen die Menue-Sequenznamen als Inline-Strings:

```
0x28FFDC "menu_logo"
0x28FFF4 "menu_title"
0x290000 "menu_name"
0x29000C "menu_finish"
0x290024 "menu_bag", "menu_invest", "menu_fukamaru", "menu_jiten" ...
```

Davor/dazwischen stehen Code-Zeiger (`0x104420`, `0x104428`, `0x104440`, `0x1045E0`, `0x1045E8`, `0x10470C`, `0x1047DC`, `0x1047E4`, ...) - offenbar eine Tabelle von Menue-Seiten mit Handlern und Sequenznamen.

**Test:** Diese Handler (`10470C/1047DC/1047E4/104420/104428/104440`) per `HLE_FRAME_LOOP2` mit `r0=0x3EE12C` aufgerufen (`tools/_seq_matrix.py`): keine `.xq`-Datei wird geoeffnet, `menu+0x74` bleibt 0. Der eigentliche Sequenz-Loader ist also nicht darunter.

**MOVW/MOVT-Scan:** `tools/_movwt.py` sucht nach gepaarten MOVW/MOVT, die eine Adresse in `0x28FF90..0x290050` bilden. Ergebnis: **0 Kandidaten** (beide Rd gleich, Abstand <= 80 B). Die Menue-Namenstabelle wird also weder per Literal noch per MOVW/MOVT adressiert; sie ist vermutlich reine Debug-/Animations-Benennung oder wird ueber eine Laufzeit-Basis (Zeiger in .data) erreicht.

**FA-Read-Pfad im Menue nicht aktiv:** Hook auf den FA-Stream-Leser `0x280744` (Build-Patch fuer `hle_fa_read`) zeigt im Menue-Lauf **0 Treffer**; nur `0x28F730` (Sync-Init) wird 2x gerufen (`out/logs/fa_log.txt`). Es werden also gar keine `.fa`-Streams gelesen. Zudem injiziert der Port bereits HID-Eingaben (START/A alternierend alle 30 Frames, `runtime/hle/hle.c` Zeile ~3386) - Eingaben sind also nicht der Blocker.

**Submodul-Matrix:** Die vier Frame-Callback-Submodule direkt aufgerufen (`tools/_sub_matrix.py`):

```
28E40C(3EC028) files=4 unk=23 dl=0
11AF00(3EC028) files=4 unk=40 dl=0
11B02C(3EC028) files=4 unk=40 dl=0
121F3C(3EC5C4) files=4 unk=32 dl=0
```

`files=4` sind nur die `gamecoin.dat`-Oeffnungen; keine Menue-Assets, `menu+0x74` bleibt 0, keine Engine-`SetCommandList`.

**600-Frame-Lauf:** Auch nach 600 Frames (~20 s) bleibt `menu+0x74=0`, `units=0`, `pool=0..0`; nur 4 `gamecoin.dat`-Oeffnungen; alle `SetCommandList` (192) mit `lr=001ED9C8` (`out/logs/600_log.txt`). Der Menue-Loop laeuft, erzeugt aber keine Inhalte.

**strh-#0x74-Scan:** `tools/_find_strh74.py` (Capstone, jede 4-Byte-ARM-Instruktion) findet **0** direkte `strh rX,[rY,#0x74]`. Der Node-Count wird also ueber eine berechnete Adresse geschrieben (kein direkter Offset), was die statische Suche nach dem Add-Node weiter erschwert.

**State-Methoden-Matrix:** Die Methoden der State-Tabelle `0x3C6FE4` (`0x37F948`, `0x37F910`, `0x376F98`, `0x37F9BC`, `0x376F70`, `0x37FBC8`, `0x298730`, `0x29880C`) wurden per `HLE_FRAME_LOOP2` mit `r0=0x3C6FE4` aufgerufen (`tools/_state_matrix.py`): alle laden nur `gamecoin.dat` (4 Oeffnungen), `menu+0x74` bleibt 0, keine Engine-`SetCommandList`. `0x29880C` laeuft ohne Null-Calls, hat aber keine Wirkung auf die Menue-Listen.

**0x284A38-Caller:** Der Ring-/Slot-Allokator `0x284A38` (von `0x28D424`/`0x2800C8` genutzt) hat 143 Aufrufer, aber **keinen** im Menue-Code (`fn_0027…`/`fn_0028…`, geprueft mit `callgraph callers --verbose`). Der Node-Add der Menue-Listen laeuft also nicht ueber diesen Allokator.

**Welt-Update ruft den Drain:** `callgraph callers 0x11DB84` zeigt: `fn_00285208` (Welt-Update) ruft bei `0x285418` die Menue-Drain-Methode `0x11DB84` -> `0x277004`. Die Kette ist also `fn_00100F98 -> fn_001034F4 -> fn_00108F70 -> fn_00285208 -> 0x11DB84 -> 0x277004`; der Welt-Update ist damit die Stelle, an der Menue-Nodes entstehen muessten (er drainiert sie direkt danach). Bei unseren direkten Aufrufen von `fn_00285208` kamen jedoch Null-Calls (`0x28EF94`).

### .fa-Registrierung wird uebersprungen (konkreter Blocker)

Der Menue-Init `fn_00100F98` erreicht im Port den Registrierblock `0x1013F0` (3x `fn_002919DC` fuer `lt6_a.fa`/`lt6_b.fa`/`lt6_uk.fa`, 1x `0x1014D0`) **nicht**. Instrumentierung mit `[flowpt]`/`[flowdbg]` in `build/gen_boot.c` (`tools/_patch_gen_flowpt.py`, `_patch_gen_flow4.py`, `_patch_gen_ldm.py`):

```
[initdbg] success
[flowdbg] block 10123C abort=0
[ldmdbg] base8=00000000 val7=00000003
[ldmdbg] passed r7=00000000 ip=000F0201 abort=0
[flowpt] after-abort-checks pc=0011A79C abort=0
[flowpt] after-stm pc=0011A79C abort=0
[flowpt] before-1034F4 pc=0011A79C abort=0
(kein [flowdbg] 2. jump-stelle, kein [initdbg] asset-block)
```

Der zweite Aufruf `fn_001034F4` bei `0x101360` kehrt nicht zurueck (er laeuft in den Welt-/Menue-Loop), also wird die Archiv-Registrierung nie ausgefuehrt. `ARM_WATCH=2919DC` ergab **0 Treffer**.

**A/B-Test:** Wird genau dieser zweite Aufruf uebersprungen (`tools/_patch_gen_skip1034.py`), laeuft der Block und die Registrierung passiert:

```
[skip1034] 2. fn_001034F4 uebersprungen
[flowdbg] 2. jump-stelle r0=00000000 abort=0 last=0011A79C
[initdbg] asset-block
ARM_WATCH=2919DC -> 3 Treffer (lt6_a/lt6_b/lt6_uk registriert)
```

Damit ist belegt: die drei echten `.fa`-Archive werden vom Spiel registriert, sobald der blockierende zweite `fn_001034F4`-Aufruf ausbleibt. Trotzdem bleiben `menu+0x6C/+0x74` leer, der FA-Reader `0x280744` hat 0 Treffer, und alle `SetCommandList` kommen weiter aus dem HLE-2D-Ring. Naechster Schritt: den blockierenden `fn_001034F4`-Zweig selbst reparieren (statt zu ueberspringen) und die Sequenz-Namenstabelle des Menue-Objekts fuellen.

Zusatzbefund: Das Menue-Objekt `0x003EE12C` hat die Vtable `0x003C6FE4` (Watch am Konstruktor `0x2980F4`: `r0=003EE12C`), nicht `0x3C6FE4` selbst. Die frueheren Methoden-Matrizen liefen mit `r0=0x3C6FE4` (Vtable) und waren damit ungueltig.


### Menue-Seiten-Setup wird per Menue-Objektzaehler uebersprungen

Korrektur zum vorherigen Befund: Die `.fa`-Registrierung wird nicht
dauerhaft uebersprungen. Mit 900 Frames (`out/logs/late_log.txt`) hat
`ARM_WATCH=2919DC` **3 Treffer** - `lt6_a.fa`, `lt6_b.fa`,
`lt6_uk.fa` werden also registriert. Der Init braucht nur lange.

Der eigentliche Blocker fuer den Menueaufbau ist der Zaehler
`menu+0x40` (`0x3EE16C`): `fn_00100F98` prueft bei `0x101508`
`fn_0010F188` = `u16[world+0x2144]` (= `menu+0x40`). Ist er != 0,
wird das Menue-Seiten-Setup `fn_0028F7BC` bei `0x10153C`
uebersprungen. Die HLE-Initialisierung setzt genau dieses Feld auf 1
(`runtime/hle/hle.c`; `HLE_MENU_OBJ0` schaltet es auf 0).

Mit `HLE_MENU_OBJ0=1` (und dem Skip des blockierenden zweiten
`fn_001034F4`, `r0=1`) laufen die echten Setup-Funktionen:

```
[flowdbg] 1014EC reached r4=00000001
ARM_WATCH=28F7BC -> 572+ Treffer
  r0=003EC028 r1=000003E8 r2=FFFFFFFF r3=FFFFFFFF lr=00101540
ARM_WATCH=2800C8 -> 600+ Treffer (Engine-Draw-Builder)
  r0=003EE12C (Menue-Objekt) r1=0CF09944 r2=00000002
```

`fn_0028F7BC` ruft also den echten Draw-Builder `fn_002800C8` mit dem
Menue-Objekt `0x3EE12C` auf. Der Builder bricht aber bei `0x280100`
ab, weil `menu+0x6A` (`0x3EE196`) = 0 ist; `HLE_DRAW_LIST=1` legt
zwar Array/Cap an (Status `dl=0/256`), aber der Count bleibt 0.

**Zusatzbefund Standardlauf:** Mit dem aktuellen Build laeuft der
Engine-2D-Renderer `fn_001ED8FC` im Normallauf jetzt mit
(`ARM_WATCH=1ED8FC` -> **280 Treffer** in 150 Frames). Der
HLE_LOG-Lauf zeigt **2703** `SetCommandList` (weiterhin
`lr=001ED9C8`, HLE-Eintrag) plus eine Engine-Liste
`addr=0x0CFE6000` (`out/logs/std3_log.txt`). Bei 150/210 Frames ist
das Bild exakt 0/96000 + 0/76800; bei 180 Frames landet der
Screenshot jetzt mitten im Frame (Tearing, weil laufend gerendert
wird).


### Draw-Builder haengt jetzt Eintraege an (menu+0x6A geklaert)

`menu+0x6A` (`0x3EE196`) ist der Freislot-Zaehler des Pools bei
`menu+0x5C`. Der Builder `fn_002800C8` liest ihn bei `0x2800F0` und
dekrementiert ihn bei `0x28015C`; der Slot selbst kommt aus
`fn_00284A38` = `[container] + [container+4]*index`. Der Konstruktor
nullt den Zaehler, die HLE-Initialisierung setzte bisher nur den
Index (`0x3EE194`) auf 1.

Mit `arm_st(0x3EE196, 0x100)` + `arm_st(0x3EE194, 0)` vor der
Init-Schleife (`build/gen_boot.c`, `0x1014EC`) und
`HLE_DRAW_LIST=1`, `HLE_MENU_OBJ0=1`, `HLE_ENGINE_MENU=1`:

```
[mw] 003EE184 <- 00000001 size=2 pc=002801A4 lr=00280158
[mw] 003EE184 <- 00000000 size=2 pc=0028DFA8 lr=0028DE78
[port] dl=0/256 arr=0CFF0000 e0=0D001E00 mn=128
```

Der Builder schreibt also einen Objektzeiger in die Objektliste
`menu+0x50` (Count bei `0x3EE184`), und der Menue-Drain konsumiert
ihn wieder. Der Zeiger `0x0D001E00` stammt aber aus dem
HLE-Scratch-Pool; das Objekt hat keine echte Klassen-Vtable.

Test mit dem echten Menue-Objekt in allen Pool-Slots
(`HLE_POOL_MENUOBJ=1`): weiterhin 192 `SetCommandList`, alle
`lr=001ED9C8` und `addr=0x0CFE6000` (HLE-Liste). Der 2D-Ring
`0x40ABC0` enthaelt also unveraendert nur den HLE-Eintrag.


### Engine-Draw-Liste erreicht Count 1, Engine sendet cmd 0x0006 selbst

Mit `HLE_ENGINE_MENU=1 HLE_MENU_OBJ0=1 HLE_DRAW_LIST=1
HLE_UNIT_XFER=1` (ohne HLE_ENGINE_3D) sind erstmals alle drei
Engine-Stufen gleichzeitig aktiv (`out/logs/ufull2_log.txt`):

```
[port] units=1 dl=1/256 arr=0CFF0000 e0=0CFF2000
[unitxfer] obj=0CFF2000 -> units=1, 3D-Renderer aufgerufen
[gsp] SetCommandList addr=0x00000001 size=0x0 lr=0034AFC4
[gsp] SetCommandList addr=0x00000001 size=0x0 lr=0034AFC4
[gsp] SetBufferSwap screen=0 fb=0x1F000000 ...
[gsp] SetBufferSwap screen=1 fb=0x1F060000 ...
```

Damit gilt erstmals:
- die Engine-Draw-Liste `menu+0x6C/+0x74` ist **nicht mehr 0** (`dl=1/256`),
- `units=1` (3D-Renderer `fn_001B9938`),
- zwei `SetCommandList` kommen aus **Engine-Code** (`lr=0034AFC4`,
  Aufrufer `fn_001B9938` bei `0x1BA3A0` -> `fn_0034AFAC`),
- `SetBufferSwap screen=0/1` wird gesetzt.

`addr=1/size=0` ist bei cmd `0x0006` aber korrekt: Azahar
(`tools/azahar/src/core/hle/service/gsp/gsp_gpu.cpp:439`) behandelt
`GSP_GPU::SetCommandList` als **Stub ohne Parameter**. Die echte
PICA-Liste laeuft ueber den GSP-Shared-Memory-CommandBuffer
(`SubmitCmdList`) + `TriggerCmdReqQueue` (`0x000C`).

Im Port ist dieser Ring aber leer: der Trigger-Log zeigt
`qops=0`, und im Bereich `0x10000000..0x10004000` gibt es keine
Gast-Schreibzugriffe. Der Engine-Renderer schreibt seine
`SubmitCmdList`-Queue also noch nicht (oder an eine andere Adresse).

**Naechster Schritt:** Den Engine-Schreiber der GSP-CommandBuffer-
Queue (`SubmitCmdList`-Eintrag) finden und dafuer sorgen, dass
`TriggerCmdReqQueue` ihn mit `qops>0` abarbeitet. Dann rendert die
Engine ihre PICA-Liste selbst; `addr=1/size=0` aus cmd 0x0006 ist
dafuer irrelevant.


### Kein SubmitCmdList-Puffer im Gast-RAM gefunden

Ein neuer Scanner (`HLE_SCAN_CMDQ`) sucht im gesamten Gast-RAM
`0x08000000..0x20000000` (0x200-Alignment) nach einem
GSP-CommandBuffer mit gueltigem SubmitCmdList-Eintrag
(`id==1`, `addr` im RAM, `size` 1..0x100000). Ergebnis im
Komplettlauf (`HLE_ENGINE_MENU=1 HLE_MENU_OBJ0=1 HLE_DRAW_LIST=1
HLE_UNIT_XFER=1`, `out/logs/cmdq2_log.txt`):

```
[cmdq] scan:
[port] units=1 dl=1/256 arr=0CFF0000 e0=0CFF2000
```

Also **0 Treffer**: Die Engine schreibt ihre PICA-Liste in keinen
GSP-CommandBuffer. Der Trigger bleibt bei `qops=0`.

### 3D-Unit-Node und Engine-Objekt (Dump)

Mit `HLE_MENU_DUMP=1` (`out/logs/ud2_log.txt`):

```
[menuobj] arr=0CFF0000 cnt=1: [0]=0CFF2000
[menuobj] obj0CFF2000: 00000000 00000000 00000002 00000000 000003E8 ...
[menuobj] unitnode: 0CFF2000 00000000 00000000 00000000 0CFF2000 0CFF2000 ...
[menuobj] units=1 u0=0CFF2000: ... 00030010 00103FA0 00104088 00104090 003EC028
```

Der Builder erzeugt also ein Objekt, das die Klassendescriptor-Worte
`0x103FA0/0x104088/0x104090` und den Weltzeiger `0x3EC028` traegt,
aber **keine Vtable** (`[obj]==0`).

### GSP-Kommando-Zensus des Engine-Laufs

`out/logs/ufull2_log.txt` (Auszug):

```
1979x gsp::Gpu cmd=0x0007 n=4   (RequestDma, Parameter p0=0 p1=1 p2=0)
 192x gsp::Gpu cmd=0x0006 n=2   (HLE-2D-Liste 0x0CFE6000/0x24)
 192x gsp::Gpu cmd=0x000C n=0   (Trigger, qops=0)
   2x gsp::Gpu cmd=0x0006 n=1   (Engine-Stub, lr=0034AFC4, addr=1/size=0)
```

Die Engine benutzt also fast nur `RequestDma` (degeneriert) und den
`SetCommandList`-Stub; der CommandBuffer-Schreiber fehlt.


### GSP-Sender-Kette und fehlender Trigger

Die RequestDma-Aufrufe (`cmd=0x0007 n=4`, p0=0/p1=1) kommen aus dem
3D-Renderer selbst: `fn_001B9938` ruft bei `0x1BA3DC`, `0x1BA5A0`
und `0x1BA624` den generischen Sender `0x24B258` -> `0x34B55C`
(Header `0x00070102`, normal=4). Die Parameter stammen aus den
Unit-Node-Feldern:

```
r0 = [node+0x10]        r1 = [node + idx*4 + 0x18]
r2 = klasse_tabelle[typ]  r3 = [[sp+0xd4]]
```

Unser synthetischer Node (`HLE_UNIT_XFER`) setzt nur `+0x10/+0x14`,
`+0x18` bleibt 0 - daher die degenerierten Parameter.

Die eigentlichen Trigger-Wrapper (`cmd 0x000C`, normal=1) sind:
`0x13E3E0`, `0x3418BC`, `0x341D34` (`out/logs/tr*.txt`).
`ARM_WATCH` auf alle drei im Komplettlauf: **0 Treffer** - die Engine
ruft ihren `TriggerCmdReqQueue` nie auf.

Am Renderer-Ende (`0x1BA954`) wird nur ein GSP-Handle geschlossen:
`ldr r0,[0x3DA6DC]; bl 0x34ABB4` -> `0x34B690` sendet `cmd 0x003A`.


### GSP-Shared-Memory-Pointer fehlt im Engine-Zustand

Messungen im Komplettlauf (`HLE_ENGINE_MENU=1 HLE_MENU_OBJ0=1
HLE_DRAW_LIST=1 HLE_UNIT_XFER=1`):

1. `svc #0x1F` (MapMemoryBlock) wird **nie** ausgefuehrt:
   `ARM_WATCH` auf `0x11F338`, `0x12E4F0`, `0x11F350` = 0 Treffer.
2. Ein erzwungener Trigger ueber `HLE_FRAME_LOOP2=3418BC`
   (`0x3418BC` = cmd `0x000C`, normal=1) laeuft 42x, liefert aber
   weiterhin `qops=0`.
3. Beide Kandidaten-CommandBuffer sind komplett null
   (`out/logs/shmd_log.txt`):
   `[cmdq] shm10000800: 00000000 ...`
   `[cmdq] shm10002800: 00000000 ...`
4. Der GSP-Modulzustand `0x3D91E8` enthaelt nur Dienstnamen:
   ```
   gspstate: 00000000 003CBF4E 003CBF45 00000003 00000000 ...
   ```
   (`0x3CBF4E`/`0x3CBF45` zeigen auf `"gsp::Gpu"`-Strings).
   `0x3D7C40` ist komplett 0; der Handle-Slot `0x3DA6DC` = `2`.
5. `0x10000000` wird nur an `0x26C620` und `0x3B4804` referenziert.

Damit fehlt der Engine der Zeiger auf den GSP-Shared-Memory-Block;
ohne ihn kann sie keinen CommandBuffer schreiben und keinen Trigger
sinnvoll absetzen.

**Naechster Schritt:** Das Engine-Global suchen, das die gemappte
Shared-Memory-Adresse aufnehmen soll (der Schreiber des
`CommandBuffer` liest es), und es auf die vom HLE benutzte Basis
setzen. Danach schreibt `GX_SubmitGxCommand` seinen
`SubmitCmdList`-Eintrag, und `TriggerCmdReqQueue` liefert `qops>0`.


### GSP-Client-Init wird im Port nie aufgerufen - Queue-Basis fehlt

`fn_00138D70` (GSP-Client-Init) hat im recompilierten Code **keinen
Aufrufer** (kein `fn_00138D70(c)` in `build/gen_boot.c`, kein BL im
gesamten Text). Im Normallauf laeuft sie also nie; bisher wurde sie
nur ueber `HLE_GSP_ENGINE` manuell angestossen.

Mit `HLE_GSP_ENGINE=1` laeuft sie und legt den Client-Kontext an
(`out/logs/ctx_log.txt`):

```
[gspengine] ctx=003D7C40 +38=00000000 ... +40=00000000 ...
            +58=00000800 +5C=00000200 +60=00000240
            +64=FFFFFFFF +68=00000001
[gspengine] 0x138D70: h10=4 h14=4 state=003D7C40 -> gsp=2
```

Bedeutung:
- `ctx+0x40` = GSP-Shared-Memory-Basis = **0**,
- `ctx+0x58` = CommandBuffer = **0x800** (Basis+0x800, also bei 0),
- `ctx+0x5C/+0x60` = 0x200/0x240 (Basis+0x200/+0x240).

Der Map-Pfad ist `0x13EAAC` -> `0x11F2C8` -> `svc 0x1F` mit
`mov r3,#0x10000000`; die Adresse landet aber nicht in `ctx+0x40`.

Setzt man per `HLE_GSP_BASE=1` die erwartete Basis:

```
[gspengine] ... base=10002000 q58=10002800
```

dann bleibt `qops=0` und die Engine schreibt dennoch keinen
`SubmitCmdList`-Eintrag (192 HLE-Listen + 2 Engine-Stubs
`lr=0034AFC4`). Es fehlt also nicht nur die Basis, sondern auch der
Aufruf des Submit-Schreibers, der `ctx+0x58` benutzt.


### Unit-Node +0x18 speist RequestDma (Feldzuordnung belegt)

Mit `HLE_UNIT_XFER` und `node+0x18 = 0x0CFF4000`
(`runtime/hle/hle.c`) zeigt der GSP-Log die Feldzuordnung:

```
[gsp] RequestDma p0=0CFF4000 p1=00000001 p2=00000000 p3=00000000
[gsp] RequestDma p0=00000000 p1=00000001 p2=00000000 p3=00000000
```

`p0` ist also exakt `[node+0x18]`; `p1` kommt aus der Klassentabelle
(`[sb+typ]` = 1), `p2/p3` bleiben 0. Die DMA hat damit `size=0` und
kopiert nichts. Die HLE-`RequestDma`-Implementierung kopiert jetzt
bei plausiblen Parametern (`src`, `dst`, `size`, `flags`), logt aber
weiterhin `qops=0` beim Trigger.

Damit ist belegt: der Unit-Node ist die richtige Stelle, aber
`+0x18` allein reicht nicht; die uebrigen DMA-Felder (Ziel und
Groesse) stammen aus einer anderen Quelle und sind im Port 1/0.


### ctx+0x58 ist der GSP-CommandBuffer (per Reset-Funktion belegt)

Die Init-Funktion ruft bei `0x13EBC4` `0x14250C(ctx+0x58, base+0x800+idx*0x200)`.
`0x14250C` ist ein **Reset des CommandBuffer-Headers**: es speichert
den Zeiger in `[ctx+0x58]` und schreibt das 32-Bit-Headerwort an
dieser Adresse mit Index/Number_commands/Status/Should_stop = 0 neu
(`ldrex/strex`-Schleife). Damit ist `ctx+0x58` eindeutig die
Azahar-`CommandBuffer`-Basis.

Der GX-Kommando-Builder liegt im Bereich `0x13F2xx` (Liste mit
0x48-Byte-Eintraegen, ruft `0x140178`, `0x142AF4`, `0x142AA8`).
`0x13F474` laeuft die Listen an `ctx+0x58`, `ctx+0x38`, `ctx+0x78`
ab und korrigiert Zeiger (Relocation).

Mit `HLE_GSP_BASE=1` wird `ctx+0x40=0x10002000` und
`ctx+0x58=0x10002800` gesetzt; der Reset `0x14250C` muss danach
laufen, damit der Header an `0x10002800` steht. Der Builder wird im
Port aber nicht aufgerufen, daher bleibt `qops=0`.

RequestDma-Feldzuordnung (Nachtrag): `p0 = [node+0x18]`,
`p1 = klasse_tabelle[typ]`, `p2=p3=0`; die HLE-DMA kopiert jetzt
bei plausiblen Parametern, im Port ist `size` aber 0.


### GX-Submit-Kette identifiziert

Die Engine-Submission laeuft ueber diese Kette (Callgraph):

```
fn_00284D24  (0x109090 / 0x109514 aus fn_00108F70)
  -> fn_0011E494  (0x2850A8)
  -> fn_0012493C  (0x124A3C)
  -> fn_0012FC24  (GX-Submit, 0x12FDA8)
  -> fn_00139914  (Builder, 0x139948)
  -> fn_0013F2D4  (Queue-Eintrag, 0x13F2D4)
```

`fn_0013F2D4(r0, r1, r2, r3, stack)` prueft `[r2] & 0x1f`, ruft
`fn_0014015C`, baut die 0x20-Byte-Eintraege und liefert 1/0.
`fn_0013F474` relokalisiert danach die Listen an `ctx+0x58`.

Im Port wird die Kette **nie** erreicht:
- `ARM_WATCH` auf `0x284D24`, `0x11E494`, `0x12493C`, `0x12FC24`,
  `0x139914`, `0x13F2D4` = **0 Treffer**.
- Ursache: `fn_00108F70` (einziger Aufrufer von `0x284D24` bei
  `0x109090`/`0x109514`) wird nicht aufgerufen, weil
  `fn_001034F4` den Aufruf bei `0x103520` nie erreicht.

Ruft man `fn_00108F70(world, world)` per `HLE_FRAME_LOOP2` auf,
laeuft `fn_00284D24` **7x** - aber der GX-Zweig (`0x11E494`) wird
nicht erreicht (`ARM_WATCH=11E494` = 0). Die Submission braucht also
die korrekten Struktur-Argumente (`r2=sp+0x50`, `r3=sp+0x3c`,
Stack `{r0,r1,r2}`) aus `fn_00108F70`.


### Echte Aufrufargumente von fn_00108F70 gemessen

An der echten Aufrufstelle (`0x101360`, zweiter `fn_001034F4`) wurden
die Argumente mitgeloggt (`build/gen_boot.c`):

```
[skip1034] 2. fn_001034F4 uebersprungen r0=003EC028 r1=0CF09A48
```

Also `r0=0x3EC028` (Welt), `r1=0x0CF09A48` (Zeiger auf einen
Init-Stack-Struct). Damit ist die Signatur bekannt:
`fn_001034F4(world, struct_ptr)` -> `fn_00108F70(world, struct_ptr)`.

Der Struct wird beim Init in einen stabilen Puffer kopiert
(`0x0CFE9800`, 0x100 Byte). Ein Aufruf
`HLE_FRAME_LOOP2=108F70, HLE_FRAME_ARG2=3EC028,0CFE9800,0,0` laesst
`fn_00284D24` laufen, aber der GX-Zweig bleibt aus:

```
[flowdbg] 1014EC reached r4=00000001
[initdbg] asset-block
[frameloop2] call 0x00108F70 r0=003EC028
ARM_WATCH=11E494 -> 0
```

`fn_00284D24` bricht also weiterhin vor `0x11E494` ab; der GX-Submit
braucht offenbar noch weiteren Zustand (geladene Szene/Ressourcen).


### fn_00284D24-Gatekette vermessen

`fn_00284D24` hat mehrere Ausstiegspunkte. Mit Instrumentierung
(`[resforce]`) und Watches auf die Blockadressen:

1. **Erstes Gate:** `bl fn_0011B98C` (0x284D60) muss != 0 liefern.
   `fn_0011B98C` -> `fn_00123780` -> `fn_0012BC58` (Rueckgabe r6 aus
   `0x289AA0`/`0x289B48`). Im Port ist das 0 ->
   `beq 0x285188` (16 Treffer).
2. Erzwingt man `fn_0011B98C -> 1`, ist `0x285188` = 0; der naechste
   Ausstieg ist `0x285194` (Gate bei `0x285048`:
   `[struct+0x4F8]` und `[struct+0x35C+0x1A4]` muessen != 0 sein).
3. Danach folgt der virtuelle Aufruf `[world]->vtable+0xA0`
   (0x285060); liefert er 0, geht es nach `0x285184` (36 Treffer).
4. Erst danach kommen `fn_0011E524` (0x285088) und der GX-Submit
   `fn_0011E494` (0x2850A8).

Der Init-Stack-Struct wird inzwischen mit 0x800 Byte nach
`0x0CFE9800` kopiert und `+0x4F8/+0x500` werden auf 1 gesetzt.
Trotzdem bleibt `ARM_WATCH=11E494` = 0.

Damit ist die Gatekette bekannt: Ressourcen-Bind (0x289AA0) ->
Struct-Felder (+0x4F8/+0x500) -> virtueller Prep-Aufruf ->
`fn_0011E524` -> GX-Submit.


### Prep-Methode ist 0x001BC450 - Kette kommt bis fn_0012493C

Der Prep-Aufruf bei `0x285060` (`[world]->vtable+0xA0`) wurde
instrumentiert (`[prep]`):

```
[prep] obj=003EC028 vt=00000000 fn=00000000
[prep] obj=003EC028 vt=003CA494 fn=001BC450
```

Die Methode ist also **`0x001BC450`** (Scene-Vtable `0x003CA494`).
Beim ersten Aufruf ist die Vtable noch 0 (Scene nicht konstruiert).

Mit erzwungenen Gates (`[resforce]` fuer `fn_00123780`,
`fn_0011B98C`, `0x285048`, Prep-Rueckgabe, `0x280034`) erreicht die
Kette:

```
ARM_WATCH=11E494 -> 7
ARM_WATCH=11E4E0 -> 3
ARM_WATCH=12493C -> 3
ARM_WATCH=12FC24 -> 0
```

`fn_0011E494` hat zwei Gates: `[obj+0x710] != 0` und
`fn_00280034 != 0` (beide im Port 0, erzwungen). Danach laeuft
`fn_0012493C`; dessen Gate `fn_00284AA8` (0x124A24) liefert 0,
daher wird `fn_0012FC24` (GX-Submit) noch nicht erreicht.

Standardpfad bleibt unberuehrt: TOP 0/96000, BOTTOM 0/76800.


### fn_0012493C-Gates: 0x26F130 und 0x130220 erzwungen

`fn_0012493C` hat mehrere `beq 0x124C2C`-Gates. Instrumentierung
(`[gx]`) im Komplettlauf:

```
[gx] 0x26F130 -> 0 (erzwungen 1)
[gx] 0x130220 -> 1 (1)
ARM_WATCH=12FC24 -> 0
```

Damit sind zwei Gates bekannt und erzwungen:
- `0x26F130` (0x124978): liefert 0,
- `0x130220` (0x124988): liefert 0.

Danach folgen weitere Gates:
`0x13C46C`/`0x284AA8` (0x124AAC), `0x1300C8`, `0x13C4EC`/
`0x284AA8` (0x124AEC), `0x130170`, danach `b 0x399F80`
(RAM-Stub -> `fn_0012FC24`).

`fn_0012FC24`/`0x13F2D4` bleiben bei 0 Treffern; `qops=0` und
192 HLE-`SetCommandList` (`lr=001ED9C8`).

Prep-Methode bleibt `[world]->vtable+0xA0 = 0x001BC450`
(Vtable `0x003CA494`).


### fn_0012493C-Gates erzwungen - fn_0012FC24 laeuft

Die vier Gates in `fn_0012493C` (`0x284AA8` bei `0x1249AC` und
`0x1249E4`, `0x1300C8` bei `0x1249C0`, `0x130170` bei `0x1249F8`)
wurden auf 1 erzwungen (`tools/_patch_gen_2493c.py`).

Ergebnis im Komplettlauf (`out/logs/c2493_log.txt`):

```
ARM_WATCH=12FC24 -> 8
ARM_WATCH=12FD38 -> 0
ARM_WATCH=139914 -> 0
ARM_WATCH=13F2D4 -> 0
Trigger ... qops=0
SetCommandList: 192, alle lr=001ED9C8
```

`fn_0012FC24` ist der **Kommando-Emitter** (schreibt PICA-Woerter
ueber `fn_00275994`/`fn_0026926C` und liefert 1). Der eigentliche
Submit-Einstieg ist die separate Funktion **`0x12FD38`**
(`bl 0x139914` bei `0x12FDA8`), die keinen statischen Aufrufer hat
und im Port nicht laeuft.

Damit ist der Emitter erreicht, aber der Submit
(`0x12FD38 -> 0x139914 -> 0x13F2D4`) noch nicht.

## Renderer-Draw-Kette live vermessen (aktueller Stand)

Der Renderer `fn_001ED8FC` ist jetzt mit echten Ring-Eintraegen
verdrahtet und die **echte Draw-Kette laeuft vollstaendig**:

```
[xfer] cont=14004440/000000B4 cnt=4 e0=003EC028 e4=003EE12C
[hook] regs addr=0010470C hits=2 r0=14004440 r1=0010470C r2=14004440 r3=00000000 lr=001ED9C8
[hook] regs addr=001105F8 hits=1 r0=00665198 r1=0CF09FD0 r2=14004440 r3=00000000 lr=001047A8
[hook] regs addr=0010470C hits=3 r0=140044F4 ... r3=00000003
[hook] regs addr=001105F8 hits=2 r0=00666618 ...
```

Damit ist die **Aufrufkonvention belegt** (vorher nur vermutet):

| Register | Wert |
| --- | --- |
| `r0` | Ring-Eintrag `0x14004440 + 0xB4*i` |
| `r1` | `[entry[0]+8]` = `0x0010470C` (Draw-Funktion) |
| `r2` | Ring-Basis `0x14004440` (von `0x284A38` in r2 belassen) |
| `r3` | Node-Index 0..3 |

Kette: `0x1ED8FC -> 0x284A38 (entry) -> 0x10470C -> 0x288960 ->
0x284A98 (Puffer) -> 0x1105F8 (Emit)`. Das passiert **4x** mit
verschiedenen Puffern (`0x665198`, `0x666618`, `0x667A98`, `0x668F18`).

**Was `0x1105F8` wirklich schreibt** (`HLE_EMIT_DUMP`):

```
[emit] buf[0]=00665198: 003C6FF8 00000000 003EC028 00000000 ... (nz=2)
[emit] buf[1]=00666618: 003C6FF8 00000003 003EC028 00000000 ... (nz=3)
[emit] buf[2]=00667A98: 003C6FF8 00000002 003EC028 00000000 ... (nz=3)
[emit] buf[3]=00668F18: 003C6FF8 00000001 003EC028 00000000 ... (nz=3)
```

Also **kein PICA-Kommandolisten-Inhalt**, sondern ein 12-Byte-
Render-Item `{0x003C6FF8, index, world=0x003EC028}`. `0x3C6FF8`
ist kein klassischer Vtable:

```
[emit] vtable 3C6FF8: 00000000 00000000 00000000 00231868 0037F948 ...
```

Deshalb liefert `gpu_run_pica_list(0x0E800000)` 0 Ops und
`gsp_process_cmdqueue` faellt auf `gsp_engine_list` zurueck
(`runtime/hle/hle.c:3433`): `nops = 1`. Das ist genau das
"Engine-Node-Liste als einen Op zaehlen", nicht eine echte
PICA-Liste.

**Verbleibender Blocker:** Die von `0x1105F8` erzeugten Render-Items
muessen von einem echten PICA-Emitter konsumiert und als nicht-leere
Liste submitted werden. Das ist der naechste konkrete Schritt.


### Ring-Entry-Layout korrigiert: {vtable, objekt}

Der Renderer `fn_001ED8FC` macht `r1=[entry]`, `r1=[r1+8]`, `blx r1`.
Der Eintrag ist also **`{vtable, objekt}`**, nicht `{objekt, objekt}`.
Mit `HLE_XFER_CLASS` schreibt der Port jetzt:

```
[xfer] cont=14004440/000000B4 cnt=4 e0=003C6FE4 e4=003EE12C
```

`e0=0x003C6FE4` ist die echte Menue-Vtable, `e4=0x003EE12C` das Objekt.
Die Loop laeuft nachweislich 4x (`0x14004440/44F4/45A8/465C`).

Achtung: Die Vtable-Werte sind **Thumb/Interworking-kodiert**. Im RAM
steht `[0x3C6FE4+8] = 0x0011DB86`, statisch in `code_decomp.bin`
`0x0011DB84`. Die ARM-Disasm bei `0x11DB84` (`push/mov/bl 0x277004/
b 0x28EF3C`) ist deshalb nur die halbe Wahrheit.

### Echte Draw-Methode und ihr Gate

`[vtable+8] -> 0x11DB84`: `bl 0x277004` (Drain) und dann
`b 0x28EF3C`. `0x28EF3C` liest:

```
r4 = entry
r0 = [entry+4]        ; = Objekt 0x003EE12C
r6 = r0 & ~3
r5 = [r6-0x10]!       ; Listwurzel bei 0x003EE11C
if (r5 == 0) return   ; sofortiger Ausstieg
```

Messung am Menue-Objekt (`[mobj]`-Dump):

```
[mobj] +00=003C6FE4  +04=00000000  +08=0CF09A78
[mobj] FFFFFFF0=00000000 ... FFFFFFFC=00000000
```

**`[0x3EE11C] = 0` und `[0x3EE130] = 0`** - die Render-Liste am
Objekt ist leer. Deshalb ist die echte Draw-Methode ein No-op, obwohl
sie korrekt aufgerufen wird.

**Naechster Schritt:** Die von `0x1105F8` erzeugten Render-Items in
die Liste bei `object-0x10` (`0x3EE11C`) einhaengen, damit `0x28EF3C`
ueber `0x27A3B4` zeichnet und submitted.


### Render-List-Gate durchbrochen: 0x27A3B4 verarbeitet den Menue-Knoten

Mit `HLE_XFER_NODE` (Knotenkopf bei `object-0x10` + Dirty-Bit) und
`HLE_NODE_CALL` (direkter Aufruf der echten Methode auf dem Ring-
Eintrag) laeuft der Engine-Consumer erstmals mit **unserem** Objekt:

```
[nodecall] 0x11DB84(entry=14004440)
[hook] regs addr=0027A3B4 hits=1 r0=003EE11C r1=003EE11C r2=00000000
       r3=00000000 r4=14004440 r5=003EE11C r6=003EE11C r7=00000000
       r8=0CF09A78 sp=0CF09FE8 lr=0028EF88
[nodecall] -> r0=003D9398 ab=0
```

- `r1=0x003EE11C` = `object-0x10`, also genau der Menue-Knoten.
- `lr=0x0028EF88` = Rueckkehr aus `bl 0x27A3B4` in `0x28EF3C`.
- Rueckgabe `0x003D9398` ist **nicht 0** (vorher: sofortiger Ausstieg
  wegen `[0x3EE11C]=0`).

Damit ist die Kette `entry{vtable,obj} -> 0x11DB84 -> 0x277004 ->
0x28EF3C -> 0x27A3B4` mit echtem Menue-Objekt belegt.

**Noch offen:** `0x27A3B4` emittiert weiterhin nichts ueber
`0x12FC24`/`0x13F2D4`; es gibt keinen `SetCommandList` an
`0x10002800` und `qops` bleibt 0. Der Frame ist unveraendert
(TOP 89358, BOTTOM 71353 im Engine-Test).


### Ursache gefunden: Vtable-Wert im RAM ist 0x0011DB86 statt 0x0011DB84

Der Renderer ruft `[[entry]]+8`; im RAM steht an `0x3C6FEC` aber
`0x0011DB86`, waehrend die statische Datei `0x0011DB84` enthaelt.
Mit `0x11DB86` landet der `blx` nicht auf der ARM-Methode (kein
0x277004/0x28EF3C). Setzt man den statischen Wert zurueck, ruft der
Renderer die echte Methode **selbst** auf:

```
[vt] [3C6FE4+8]=0011DB84 entry0=003C6FE4 entry4=003EE12C
[hook] regs addr=0027A3B4 hits=1 r0=003EE11C r1=003EE11C r4=14004440 lr=0028EF88
[hook] regs addr=0027A3B4 hits=2 ... r4=140044F4 lr=0028EF88
[hook] regs addr=0027A3B4 hits=3 ... r4=140045A8 lr=0028EF88
[hook] regs addr=0027A3B4 hits=4 ... r4=1400465C lr=0028EF88
```

Also **ohne** `HLE_NODE_CALL`: Der Renderer-Loop treibt die echte
Kette `0x11DB84 -> 0x277004 -> 0x28EF3C -> 0x27A3B4` fuer alle vier
Menue-Eintraege (`r4` = jeweiliger Ring-Eintrag).

Der globale Callback `0x192D4C` ([0x3D8F40]+4) laeuft 1x, ist aber
reine Vektor-/Matrixarbeit (`0x27AA38` min, `0x28A600` sub). Es gibt
weiterhin **0 Hits bei `0x12FC24`**, keinen `SetCommandList` und
`qops=0`; der Frame bleibt 89358/71353.


### "Nur 1 von 4" geloest: geteiltes Objekt + Dirty-Bit

Alle vier Ring-Eintraege zeigten auf **dasselbe** Menue-Objekt
`0x3EE12C`. `0x273470` loescht beim ersten Draw das Dirty-Bit
(`bic r3,r3,#1` bei `0x2734E0`), daher nahmen die Eintraege 2..4 den
"Nicht-Dirty"-Pfad (`0x27A468`) und riefen den Callback nicht.

Mit `HLE_XFER_SPLIT` bekommt jeder Eintrag eine eigene Objektkopie
(`0x0CFFC000/100/200/300`, je eigene Knotenwurzel + Dirty-Bit). Ergebnis:

```
[vt] [3C6FE4+8]=0011DB84 entry0=003C6FE4 entry4=0CFFC000
[hook-final] count addr=0027A3B4 hits=5      (4x Menue + 1x Welt)
[hook-final] count addr=00192D4C hits=4      (Callback 4x)
```

Damit ist die Ursache belegt und behoben: **4/4** Knoten erreichen
den Vektor-/Matrix-Callback, vorher nur 1/4.

`0x193A94` und der Emitter `0x12FC24` haben weiterhin 0 Hits; es gibt
keinen `SetCommandList`/`qops`, der Frame bleibt 89358/71353.


### Korrektur: 0x28EF3C -> 0x27A3B4 ist ein Unlink-Pass, kein Draw

`[0x3D8F40]+4` zeigt auf **`0x00192D4C`**, und `code_decomp.bin` hat
dort `E12FFF1E` = **`bx lr`** (verifiziert auch gegen das frisch
dekomprimierte `code.bin`: orig == cur == `E12FFF1E`). Die echte
Funktion beginnt erst bei `0x192D54`.

`0x27A3B4` ruft also einen **No-Op-Callback** und loescht danach
Flags/Listeneintraege (`0x273470`, `bic r3,r3,#1`). `0x28EF3C` ist
damit ein Flush/Unlink-Schritt, **keine Zeichen-/Emitter-Funktion**.
Der zuletzt verfolgte Pfad ist fuer die PICA-Emission eine Sackgasse.

### Vtable-Wert: statisch 0x0011DB84, zur Laufzeit 0x0011DB86

Vergleich `decompress_code(code.bin)` vs. RAM:

```
0x003C6FEC: orig/cur = 0011DB84   (0x0011DB86 kommt im ganzen Image NICHT vor)
```

Der `+2`-Wert entsteht erst zur Laufzeit: nach dem internen
`HLE_DRAW_DIRECT`-Aufruf von `0x10470C` steht dort `0x0011DB86`
(vorher/nach dem Schreiben `0x0011DB84`). Ein erzwungener Wert
`0x11DB84` vor dem Renderer-Call laesst die Kette
`0x11DB84 -> 0x277004 -> 0x28EF3C -> 0x27A3B4` laufen.

**Konsequenz:** Der echte Menue-2D-Draw liegt nicht in diesem
Unlink-Pfad; er muss ueber den Drain/Builder `0x277004` bzw. den
HLE-2D-engine2d-Pfad gesucht werden.


### Submit-Kette ist rein dynamisch dispatcht (kein statischer Aufrufer)

Scan ueber `code_decomp.bin` (ARM **und** Thumb, alle `bl/b/blx`):

```
12493C: 0 Aufrufer      12FD30: 0      12FD38: 0
139914: 0               13F2D4: 0      12FC24: 0
```

Auch als Datenwort (`0x0012493C`/`0x0012FD30`/...) kommt im Image
**nichts** vor. Die Submit-Kette wird also ausschliesslich ueber
dynamischen/virtual Dispatch erreicht.

Kontrolle mit `HLE_RETFORCE` auf die vier `fn_0012493C`-Gates
(`284AA8=1,1300C8=1,130170=1`, `tools/run_emitter_gate.py`): `0x12493C`
wird **0x** erreicht, `0x12FC24`/`0x12FD30`/`13F2D4` ebenfalls 0,
`qops=0`. Gates allein starten den Submit also nicht - der
**Dispatch-Eintrag** (Vtable-Slot, vom echten Loader gesetzt) fehlt.

**Konsequenz:** Naechster sinnvoller Schritt ist, den Loader-/Scene-Pfad
zu finden, der `fn_0012493C` als virtuellen Slot registriert und
aufruft; erst danach kann der Emitter eine echte PICA-Liste bauen.


### Dynamischer Dispatch-Eintrag gefunden (Roh-Branch-Scan)

Ein Roh-Scan der ARM-Branch-Encodings (statt Capstone-Linear-Disasm)
liefert die komplette Kette zum Emitter/Submit:

```
0x100B8C -> 0x100F98 -> 0x1034F4 (bl 0x108F70 @0x103520)
         -> 0x108F70 -> 0x284D24 -> 0x2850A8
         -> 0x11E494 (r0=[world+0xBC4])
         -> 0x280034 (Container) -> 0x12493C
         -> 0x12FC24 (Emitter, @0x124A3C) + 0x12FD30 (Submit @0x124ACC)
         -> 0x139914 (@0x12FDA8) -> 0x13F2D4 (@0x139948)
```

- `0x12493C` wird von `0x11E4EC` gerufen, dieser von `0x2850A8`.
- `0x11E494` ist der gesuchte Dispatch-Eintrag; `r0=[world+0xBC4]`
  (GX-Objekt), das im Port nie gesetzt wird.
- Gate in `0x108F70`: bei `0x109004` prueft `0x11B530`; ist das
  Ergebnis 0, springt der Code nach `0x1094CC` und laesst den
  Render-Zweig `0x109090` aus.

**Messung (`tools/run_dispatch_chain.py`, `out/logs/dispatch_chain.txt`):**

```
[initcall] r0=003EC028 r1=0CF09A48 r3=001013C7 lr=00101368
[hook-final] count addr=001034F4 hits=1
[hook-final] count addr=00108F70 hits=1
FEHLT 11B530 / 109090 / 284D24 / 11E494 / 12FC24 / 12FD30
qops=0
```

Also: Init-Call und GX-Renderer-Entry **laufen**, aber `0x108F70`
verlaesst den Render-Zweig vor `0x11B530`/`0x109090` (Abbruch oder
Gate). Das ist der naechste, jetzt exakt lokalisierte Blocker.


### Engine-2D-Renderpfad laeuft jetzt (Gates 0x1BC450/0x11B530)

Mit `HLE_RETFORCE="1BC450=3EE12C,11B530=1"` und `[menu+0x710]=1`
laeuft der echte Render-Zweig in `0x108F70`:

```
[engine] fn_0028F084(003EC028) gestartet, [scene]=00000000
[engine] Menue-Assets geladen (top=0D000000 bottom=0D050000)
[tex] 0x132370(tex=0CFE7100) -> r0=1
[tex] 0x27B34C bind -> r0=0
[engine] 2D-Displaylistentry installiert (entry=0CFE3000 arr=14004440 ...)
[engine2d] draw konsumiert tex=0CFE7100 src0=1E000000 src1=1E100000
[tex] 0x2812EC(sprite) -> r0=0
[engine2d] Sprite-Entry idx=0 submitted (lr=001ED9C8)
[gsp] SetCommandList addr=0x0CFE6000 size=0x24 lr=001ED9C8
[gsp] Trigger ... qops=0
```

Der Engine-Pfad bindet also eine Textur und submitted einen Sprite.
**Aber:** submitted wird die HLE-Stub-Liste `0x0CFE6000` (0x24 B),
daher `qops=0`; `[scene]=00000000` ist weiterhin leer, und der
Top-Frame wird schlechter (90281 statt 65962), weil der Engine-Draw
den Host-Blend ersetzt.

**Naechster Schritt:** `[scene]` mit dem echten Menue-Scene-Objekt
belegen und die Sprite-Entry-Liste (`entry=0CFE3000`) so fuellen, dass
`0x12FC24` echte PICA-Woerter schreibt und `0x12FD30` sie (statt der
HLE-Stub-Liste) submittet.


### [scene] gebunden: Scene-Konstruktor laeuft, 4 Sprite-Eintraege submitted

Mit `HLE_ENGINE_FRAME=1`/`HLE_ENGINE_RES=1` laeuft `hle_engine_frame`:

```
[engine] fn_0028F084(003EC028) gestartet, [scene]=00000000
[engine] 2D-Displaylistentry installiert (entry=0CFE3000 arr=14004440 ...)
[engine] Scene-Konstruktor 0x381CE4 aufrufen
[engine] nach Konstruktor [scene]=003CA494
[hook-final] count addr=00381CE4 hits=1
[hook-final] count addr=0028D424 hits=6516
[engine2d] Sprite-Entry idx=0 submitted (lr=001ED9C8)
[engine2d] Sprite-Entry idx=1 submitted
[engine2d] Sprite-Entry idx=2 submitted
[engine2d] Sprite-Entry idx=3 submitted
```

Damit ist `[scene]` **nicht mehr 0**, sondern `0x003CA494`; der
Menue-Ressource-Loader `0x28D424` laeuft 6516x, und der Engine-2D-Pfad
submittet jetzt **vier** Sprite-Eintraege (vorher nur idx=0).

Weiterhin offen: submitted wird die HLE-Stub-Liste `0x0CFE6000`
(`qops=0`); `0x11E494`/`0x12493C`/`0x12FC24`/`0x12FD30` haben 0 Hits.
Frame: TOP 90281/90400, BOTTOM 26091/71360.


### Emitter 0x12FC24 erstmals mit korrekten Args aufgerufen

`tools/run_chain_direct.py` ruft den Orchestrator `0x12493C` direkt auf
(Gates per HLE_RETFORCE, Buffer 0x0D100000). Ergebnis:

```
[hook] regs addr=0012FC24 hits=1 r0=003EE460 r1=003EE2D0 r2=0D100000
       r3=00001000 r4=00001000 r6=003EE12C r8=003EC028 lr=00124A40
[hook-final] count addr=0012493C hits=1
[hook-final] count addr=0012FC24 hits=1
```

Also genau die erwarteten Argumente:
- `r0=menu+0x334` (0x003EE460), `r1=menu+0x1a4` (0x003EE2D0)
- `r2=0x0D100000` (Zielpuffer), `r3=0x1000` (Groesse)

Der Emitter ruft danach `0x284AA8`/`0x279E50` und baut 0x28-Byte-Nodes;
mit der erzwungenen Groesse 0x1000 laeuft er ueber das Frame-Budget
(Saved PC `0x2896C4`). `0x12FD30`/`0x13F2D4` werden daher noch
nicht erreicht; `qops=0`.

**Naechster Schritt:** die echte Groesse aus `0x132FF8` (statt
`=1000`) liefern und `0x284AA8` mit einem gueltigen Container natuerlich
allokieren lassen, damit die Node-Schleife terminiert und
`0x12FD30 -> 0x139914 -> 0x13F2D4` submitted.


### Emitter-Args bestaetigt; Node-Schleife terminiert nicht im Call-Budget

`0x132FF8` = `4 + 4*count(0x140140(menu+0x1a4))`. Mit realistischer
Groesse `r3=0x14` laeuft `0x12FC24` mit korrekten Args:

```
[hook] regs addr=0012FC24 hits=1 r0=003EE460 r1=003EE2D0 r2=0D100000
       r3=00000014 r4=00000014 lr=00124A40
[port] frames=29 pc=0028990C ...
[port] frames=59 pc=002896C4 ...
```

Der Emitter baut danach 0x28-Byte-Nodes (`0x2896B8..0x2896F0`,
`0x279E50`/`0x284AA8`) und kehrt innerhalb des HLE-Call-Budgets nicht
zurueck (PC bleibt in `0x2896xx`). `0x12FD30`/`0x13F2D4` laufen daher
noch nicht, `qops=0`.

**Naechster Schritt:** Call-Budget von `arm_call_guest_pub` fuer
`0x12493C` anheben (oder Loop-Zaehler `r4` aus dem echten
`0x140140`-Ergebnis speisen), damit der Emitter fertig wird und der
Submit schreibt.


### Node-Schleife laeuft (r4=0x20), Submit noch nicht

Entgegen der Unterlauf-Vermutung laeuft die Node-Schleife mit
`r4=0x20` (32 Items), Puffer `0x0D100000`:

```
[hook] regs addr=002896B8 hits=1 r0=0D100000 r1=00000500 r2=00000002 r4=00000020 r5=0D100000
[hook-final] regs addr=002896B8 hits=16120
```

Der Emitter baut also real Nodes (16120 Schleifendurchlaeufe), aber
`0x12FD30` hat weiterhin 0 Hits. Der Submit-Zweig (ab `0x124A48`
`0x133018`/`0x133150`/`0x284AA8`) bricht also nach dem Emitter ab.


### Emitter kehrt nicht zurueck: Post-Emitter-Zweig unerreicht

`0x12FC24` wird korrekt gerufen, aber die nachfolgenden Funktionen des
Orchestrators werden nie erreicht (immediate Regs-Hooks):

```
[hook] regs addr=0012FC24 hits=1 r0=003EE460 r1=003EE2D0 r2=0D100000 r3=00000014 lr=00124A40
(keine Hits) 0x133018 / 0x133150 / 0x12FD30
[port] frames=59 pc=002896C4
```

Der Node-Fill-Loop `0x2896B8` laeuft mit `r4=0x20..` und
`r5=0D100000, 0D100028, ...` (echte 0x28-Schritte), aber **16120**
Durchlaeufe: der aeussere Loop in `0x275994` terminiert nicht. Dadurch
kehrt `0x12FC24` nicht zurueck, der Submit-Zweig ab `0x124A48`
(`0x133018`/`0x133150`/`0x12FD30`) wird nie erreicht; `qops=0`.

**Naechster Schritt:** den aeusseren Loop in `0x275994`/`0x2896xx`
terminieren lassen (Listen-Terminator/Count korrekt setzen), damit
`0x12FC24` zurueckkehrt und `0x12FD30 -> 0x139914 -> 0x13F2D4` den
Queue-Eintrag an `0x10002800` schreiben.


### Submit 0x12FD30 erstmals erreicht (Emitter-Helper 0x275994 uebersprungen)

Mit `HLE_RETFORCE=...,275994=1` kehrt `0x12FC24` zurueck und der
Orchestrator laeuft bis zum Submit:

```
[hook] regs addr=0012FD30 hits=1 r0=003EE680 r1=003EE2D0 r2=003EE460
       r3=0D100000 r4=00000040 r5=00000040 r6=003EE12C r7=0D100000
       r8=003EC028 sp=0CF09EF0 lr=00124AD0
[hook-final] regs addr=0012FD30 hits=20
```

Argumente sind korrekt: `r0=menu+0x554` (CommandBuffer),
`r1=menu+0x1a4`, `r2=menu+0x334` (Descriptor), `r3=0x0D100000`
(Emitter-Puffer). Das ist der erste Lauf, in dem der Submit-Einstieg
mit plausiblen Args erreicht wird.

`0x139914`/`0x13F2D4` laufen aber noch nicht. Gate-Analyse von
`0x12FD30`:

1. `bl 0x139844` (SDK-Flag `0x3E7771`); bei 0 -> Bail `0x12FDE4`.
2. danach Groessenpruefung `0x12FD68..0x12FD90` mit Stack-Args
   `[sp+0x68/0x6c/0x70]`; bei Ueberschreitung -> Bail `0x12FDE0`.
3. erst dann `bl 0x139914` bei `0x12FDA8`.

Der Submit bricht also an (1) SDK-Flag und/oder (2) Stack-Groessen ab.
`qops=0`.


### Submit-Kette 0x139914 + 0x13F2D4 laufen (20x) mit echten Args

Nach Bedienen der Groessen-Gates in `0x12FD30`:

```
[retforce] hit 00283160 -> r0=0E000000 (lr=0012FD74)
[retforce] hit 002691DC -> r0=01000000 (lr=0012FD7C)
[hook] regs addr=0012FD94 hits=1 r0=0E800040 r1=0F000000 ...
[hook] regs addr=00139914 hits=1 r0=003EE680 r1=003EE2D0 r2=0E800000 r3=00000040 lr=0012FDAC
[hook] regs addr=0013F2D4 hits=1 r0=003EE680 r1=003EE2D0 r2=0D1FFE08 r3=0E800040 lr=0013994C
[hook-final] regs addr=0013F2D4 hits=20
```

Damit laufen erstmals:
- `0x139914` mit `r0=CommandBuffer 0x3EE680`, `r2=Liste 0x0E800000`, `r3=0x40`
- der Submit-Schreiber `0x13F2D4` (20x)

Gates, die dafuer noetig waren und vorher 0 lieferten:
- `0x283160` (Arena-Basis) -> `0x0E000000`
- `0x2691DC` (Arena-Groesse) -> `0x01000000`
- `0x139844` (SDK-Flag) -> 1
- `0x275994` (Emitter-Helper, haengt) uebersprungen

`qops` bleibt 0, weil in diesem Harness (`HLE_FRAME_LOOP2=12493C`)
der Engine-`TriggerCmdReqQueue` nur fuer `fa2==0x12FD30` ausgeloest
wird; die Sichtbaren Trigger sind die HLE-2D-Stub-Liste `0x0CFE6000`.


### Submit-Body laeuft vollstaendig; GSP-Bruecke fehlt noch

Mit Arena-Gates (`283160=0E000000`, `2691DC=01000000`), SDK-Flag
(`139844=1`) und `275994=1` laeuft die Kette jetzt bis in den
Queue-Writer:

```
[hook] regs addr=00139914 hits=1 r0=003EE680 r1=003EE2D0 r2=0E800000 r3=00000040 lr=0012FDAC
[hook-final] regs addr=0013F2D4 hits=20
[hook-final] count addr=00139954 hits=19
[hook-final] count addr=0013A854 hits=19
```

`0x12FD30` kehrt mit 1 zurueck und setzt den Command-Buffer-Header
(`[r4+0x98]=r4+0x9c`, `[r4+0x20]=sb`). Aber der GSP-Queue-Eintrag ist
leer:

```
[queue] pre-trigger n=0 addr=00000000 size=0 desc+4=00000000 A=00000000
```

`Sichtbar` qops=0 (nur HLE-2D-Stub-Liste `0x0CFE6000`). Die Engine
schreibt also ihren internen Command-Buffer (`0x3EE680`), aber noch
nicht `0x10002818/0x1C/0xB0`. Der HLE-Trigger laeuft jetzt auch fuer
`fa2==0x12493C`.


### Praezisierung: 0x0E800000 bleibt leer (Emitter-Body uebersprungen)

Mit `275994=1` laufen `0x139914`(19x), `0x13F2D4`(20x), `0x13A854`(19x)
und `0x12FD30` setzt den Command-Buffer-Header, aber die Zielliste ist
leer:

```
[queue] pre-trigger n=0 addr=00000000 size=0
[qmem] E800000: 00000000 00000000 ... (16 Worte null)
```

Grund: `0x275994` (der die PICA-Woerter erzeugt) wurde uebersprungen.
Ohne dieses Gate kehrt `0x12FC24` nicht zurueck. Damit ist der
GSP-Queue-Eintrag noch leer und `qops=0`.


### Emitter-Ziel bestaetigt: r2=0x0E800000, r3=0x14

`0x12FC24` erhaelt jetzt den korrekten Zielpuffer (ohne `275994=1`):

```
[frameloop2] call 0x0012493C r0=003EE12C
[hook] regs addr=0012FC24 hits=1 r0=003EE460 r1=003EE2D0 r2=0E800000
       r3=00000014 r4=00000014 r8=003EC028 lr=00124A40
```

Die Submit-Plumbing (`0x139914`/`0x13F2D4`/`0x13A854`) laeuft, der
Zielpuffer `0x0E800000` bleibt in der Beobachtung aber leer
(`[qmem] ... 00000000`), Queue `n=0`, `qops=0`. Der Emitter schreibt
seinen Count/Header offenbar nicht an die beobachtete Adresse bzw.
wird vorher abgebrochen; die genaue Schreibadresse muss per
`0x12FC80`-Beobachtung (r1-Wert) verifiziert werden.


### Emitter-Size-Gate war der Blocker (nicht 0x275994)

Mit `HLE_EMITDUMP` an `0x12FC24`:

```
[emitin] r2=0E800000 r3=00000014 dsc[3EE678]=00000000 E800000: 0...0
```

`0x12FC24` berechnet `needed = 4+4*count(0x140140(menu+0x1a4))`.
Der erzwungene Wert `132FF8=0x14` passte nur bei count==4; bei
count>4 bricht `0x12FC54..0x12FC68` (`bgt 0x12FD2C`) ab, bevor etwas
geschrieben wird. **`132FF8` entfernt** -> self-consistent:

```
[emitin] r2=0E800000 r3=00000004   (count==0, needed==4)
```

Jetzt laeuft der Emitter durch (kein Bail). Er schreibt nur `count=0`,
weil die Menue-2D-Liste `menu+0x1a4` in dieser Konfiguration leer ist
(`0x140140` liefert 0). `0x0E800000` bleibt daher leer, Queue `n=0`,
`qops=0`.

**Naechster Schritt:** `menu+0x1a4` fuellen (Engine-Ressource-Liste mit
den 4 Sprite-Eintraegen), sodass `0x140140>0`, der Emitter echte
PICA-Woerter schreibt und `qops>0` entsteht.


### Count-Kette gefunden: [0x3EE2D4]=0 war die Ursache fuer count=0

`0x140140(menu+0x1a4)` folgt `[menu+0x1a4+4] -> +0x3C -> [..]`.
Dump am Emitter:

```
[emitin] chain [3EE2D4]=00000000 o2=00000000 count=00000000
```

Also war `[0x3EE2D4]` (das Objekt hinter menu+0x1a4) **0**, daher
`count=0` und der Emitter schrieb nichts.

Mit einer minimalen gueltigen Deskriptor-Kette
(`HLE_POKE 3EE2D4=0D300000, 0D30003C=0D300100, 0D300100=1`):

```
[emitin] chain [3EE2D4]=0D300000 o2=0D300100 count=00000001
[emitin] r2=0E800000 r3=00000008
```

Count=1 -> `needed=8`, Size-Gate besteht. **Aber** mit count>0
betritt der Emitter den echten Node-Builder und kehrt nicht zurueck
(PC `0x26927C`/`0x2896C4`), sodass `0x139914` noch nicht erreicht wird
und `qops=0` bleibt.


### Emitter schreibt erstmals in die Zielliste (count=1) und kehrt zurueck

Mit gueltiger Count-Kette (Poke `3EE2D4=0D300000, 0D30003C=0D300100,
0D300100=1`) und `HLE_RETFORCE 1433A4=0,26926C=1` (Node-Builder, der
sonst haengt, uebersprungen):

```
[postemit] E800000: 00000001 00000000 ... 
[hook-final] count addr=00139914 hits=19
[hook-final] count addr=0013F2D4 hits=19
```

Erstmals:
- Der Emitter `0x12FC24` schreibt **non-zero** in die Zielliste
  (`0x0E800000 = 1`) und **kehrt zurueck**.
- `0x139914` (19x) und `0x13F2D4` (19x) laufen mit dieser Liste.

Weiterhin offen: `[queue] pre-trigger n=0` (GSP-Queue `0x10002818`
nicht geschrieben), `qops=0`. Der Emitter-Node-Builder (`0x26926C`,
Liste ueber `0x1433A4`) liefert mit synthetischen Daten keine echten
PICA-Kommandos, daher enthaelt die Liste nur den Count-Header.


### SubmitCmdList wird gelesen, aber ops=0 (Liste ohne PICA-Paare)

Mit einem SubmitCmdList-Eintrag auf die Engine-Liste:

```
[gspq] tid=0 idx=0 id=1 list=0E800000 size=8 ops=0
```

`gsp_process_cmdqueue` liest den Eintrag korrekt, aber
`gpu_run_pica_list(0x0E800000,8)` findet **keine** PICA-Paare, weil die
Liste nur den Count-Header (`00000001`) enthaelt (Node-Builder
uebersprungen). Damit ist die letzte Luecke exakt: der Node-Builder
muss echte `(value,header)`-Paare schreiben.


### Command-Buffer 0x3EE680 ist bei 0x139914 noch leer

Erweiterter `[postemit]`-Dump:

```
[postemit] E800000: 00000001 0..0 | CB3EE680: 00000000 ...
```

Also enthaelt weder die Zielliste (nur Count=1) noch der Engine-
Command-Buffer (`0x3EE680`) PICA-Paare zum Submit-Zeitpunkt. Der
Node-Builder `0x26926C` (Ring-Manager `0x1433A4`/`0x266858`) muss die
`(value,header)`-Paare erzeugen; mit synthetischen Daten tut er das
nicht.


### Ring-Manager 0x42B7AC ist komplett uninitialisiert

Dump am Emitter:

```
[ring42B7AC] +17C=0 +180=0 +184=0 +188=0 +18C=0 +190=0
             p20=0 p24=0 p28=0 p2C=0 p30=0
```

Der Node-Builder `0x26926C` arbeitet also auf einem **genullten**
Ring-Manager (Basis/Ende/aktuell/Flag und Pool alle 0). Selbst nach
Initialisierung dieser Felder per Poke (`+17C=0D400000`, `+188=0x200`)
terminiert der Builder vor `0x139914` nicht (kein `[postemit]`).

Die Queue-Verarbeitung selbst funktioniert nachweislich:

```
[gspq] tid=0 idx=0 id=1 list=0E800000 size=8 ops=0
```

`gpu_run_pica_list` findet aber keine Paare, weil `0x0E800000` nur
Count=1 enthaelt.


### Ausgabepfad 0x26926C analysiert: Warteschleife im leeren Pool

`0x26926C` terminiert nicht, weil nach `0x269300` (`0x26E1FC`) die
Schleife

```
00269310 add r0,r4,#0xb4
00269314 bl 0x2668E4
00269318 ldr r3,[r0,#8]
0026935C bne 0x269310
```

auf `[node+8] == r5` wartet, aber der Pool `ctx+0xb4` (Felder
`+0x20/+0x24/+0x28`) im Port komplett genullt ist. Damit laeuft die
Schleife endlos und `0x12FC24` kehrt nicht zurueck (kein `[postemit]`).

Zusaetzlich: `0x1433A4` nutzt den Ring `+0x17C` (Basis), `+0x180`,
`+0x184` (aktuell), `+0x188` (Ende), `+0x18C` (Flag) - alle 0.

**Naechster Schritt:** Pool `ctx+0xB4` mit gueltigen Eintraegen
(Basisarray, `+0x20` Maske, `+0x24` Index, `+0x28` Anzahl) und Ring
`+0x17C..0x18C` initialisieren, sodass `0x2668E4`/`0x269310`
terminieren und die PICA-Paare geschrieben werden.


### Lazy-Init-Gate 0x28AAF0 gefunden (Skill-Workflow guest-refs)

`guest-refs 0x0042B7AC` liefert nur `fn_00275994` (Lazy-Singleton).
Dort:

```
2759AC blx 0x28AAF0
2759B0 cmp r0,#0
2759B4 beq 0x2759D4      ; wenn 0 -> Ring bleibt genullt
2759BC bl 0x13038C       ; Ring-Init
```

Mit `HLE_RETFORCE 28AAF0=1` laeuft `0x13038C`, der Ring wird
initialisiert (Pokes `+17C=0D400000`, `+188=0x200` bleiben), und
`0x12FC24` kehrt zurueck (`[postemit]` erscheint). Der Engine-
Command-Buffer erhaelt Daten:

```
CB3EE680: 003ECB68 00000000 00000018 00000000 00000000 00000004 ...
```

Aber: `[gspq] id=1 list=003EE680 size=20 ops=0` - `gpu_run_pica_list`
dekodiert dieses Engine-Format nicht als `(value,header)`-Paare
(Header-Woerter sind 0). `qops=0`.


## Objektklasse des Engine-CommandBuffers identifiziert

`guest-refs 0x003C91EC`: Vtable, Konstruktor `fn_0030F944`
(`str r1,[r0]` mit Pool `0x003C91EC`). Vtable-Slots:

```
+00=0030F97C  +08=00124FA8  +0C=0036CCF0  +10=00269E90
+14=00297B24  +18=00297618  +2C=0030FD30  +38=0030FD48  +3C=001BD9E4
```

- `+08 = 0x124FA8 -> 0x27A7B8` ist ein **Reset/Unlink** (loescht Felder),
  kein Draw.
- `+14 = 0x297B24` iteriert die Einertraege (`+0x78`, Loop nach
  `0x297D38`) und ist der naechste Draw-Kandidat.

Objekt `0x3ECB68` (Vtable 0x3C91EC) traegt Name ~„…Obj2DManager" und
Count-Felder 4. `HLE_PRECALL=297B24,3ECB68,...` lief timing-flaky
(`fl2` feuert nur alle 4096 Ticks), daher noch nicht als Draw belegt.


### Deterministische Kette: HLE_FL2_MASK + HLE_POSTCALL

`fl2` feuerte nur alle 4096 Ticks (Flakiness). Neu:
`HLE_FL2_MASK=0` -> Frame-Loop jeden Tick. `HLE_POSTCALL=addr,r0..r3`
ruft deterministisch nach `0x12493C`.

```
[postcall2] 0x00297B24(003ECB68) -> r0=00000001 ab=0
[dumpmem2] 0E800000: 00000001 0...
[gspq] ... ops=4
[gsp] Trigger ... qops=4
```

`0x297B24` (Vtable+0x14) laeuft also deterministisch und erfolgreich,
schreibt aber **nicht** nach `0x0E800000`. Die 16-Zeilen-Kette ist nun
reproduzierbar (kein Timing-Glueck mehr).


### guest-watch laeuft jetzt mit derselben Exe/Args wie der Runner

Vorher: `guest-watch` ohne `--exe/--arg` -> 0 Ereignisse auf 0x0E800000.
Jetzt (`tools/watch_chain.py` gibt `--exe out\port.exe --arg out\romfs
--arg=--shot ...` mit):

```
write 0x0E800000  0x00000001  0x001EDA0C  lr=0x00140154  fn_001ED8FC+0x110
write 0x0E800000  0x00000001  0x00143F60  lr=0x00140154  fn_00143F60
```

Damit sind Messlauf und Watch-Lauf konsistent (Reproduzierbarkeit).


### Klasse 0x3C91EC: Slot-Karte (decodiert)

`+0x10 = 0x269E90` (Lock/Iteration; Body 0x269E94, ruft 0x283AE8/0x266680)
`+0x14 = 0x297B24` (Reset/Iteration; ruft 0x269E94/0x231E6C/0x28796C)
`+0x18 = 0x297618` (Draw-Buffer-Builder; nutzt 0x284AA8, 0x284A48,
         0x288890, 0x288938 - dieselbe Allokator-Familie wie 0x10470C)
`+0x0C = 0x36CCF0` (Kleinkram/Hash), `+0x2C/+0x38/+0x3C` weitere.

Aufruf von `0x297618(0x3ECB68)` per HLE_POSTCALL: `r0=0` (leer/
fehlende Ressource), schreibt nicht nach 0x0E800000. `0x297B24` liefert
`r0=1` und schreibt nur Nullen in Objektfelder.

Damit ist die Klasse strukturell bekannt; der GPU-Emitter liegt hinter
`+0x18`/0x284AA8-Allokationen und braucht echte Element-Ressourcen.


### 0x297618 erzeugt echte Engine-Eintraege in 0x0E800000

Mit gueltigem Count-Zeiger (`r2=0x0D300100`, `[r2]=4`) und Container
(`r1=0x3ECB68`):

```
[postcall2] 0x00297618(003ECB68,003ECB68) -> r0=0 ab=0
[dumpmem2] 0E800000: 00000004 0 0 0 0 0 003CAC3C 0 0000FFFF 0 0 0
                     003CAC3C 0 0000FFFF ...
```

Also: Count 4 plus wiederholte Eintraege `{ptr=0x3CAC3C, 0, mask=0xFFFF}`.
`0x3CAC3C` ist eine Vtable mit `+8 = 0x2975AC` (Entry-Draw):

```
002975AC: if ([obj+0x14]) [obj+0x14]->[[vt]+8]() ; Felder reset ; b 0x28EF3C
```

Der Entry-Draw delegiert an ein **Kind-Objekt bei `+0x14`** und endet im
bekannten Unlink-Pass `0x28EF3C`. Solange `+0x14` leer ist, gibt es kein
PICA-Kommando. Der Builder selbst liefert `r0=0` (Status), schreibt aber
die Eintraege.


### Blocker exakt: entry+0x14 (Kind-Sprite) wird nie gesetzt

`guest-watch 0x0E80002C` (entry0+0x14) mit aktivem Builder:

```
5 Schreibzugriffe, ALLE Wert 0x00000000
  pc=0x00288890 (fn_00288890) lr=0x002976F8  [Builder 0x297618+0xE0]
  pc=0x00279E50 (fn_00279E50) lr=0x002896C4  [0x2895D0/Fill]
```

Der Builder initialisiert `+0x14` also **explizit mit 0**; in unserem Lauf
setzt **kein** Engine-Code ein Kind-Sprite-Objekt. Damit ist
`0x2975AC` korrekt, aber ohne Draw-Ziel -> keine PICA-Paare.

Das ist die praezise fehlende Ressourcen-Bindung: das Kind-Objekt an
`entry+0x14` (Sprite) wird vom Resource-Loader nicht erzeugt/verknuepft.


### HLE_POSTCALL als Sequenz + Kind-Objekt gesetzt

`HLE_POSTCALL` akzeptiert jetzt `addr,r0..r3;@addr=val;...`.
Damit deterministisch:

```
[postcall2] 0x00297618(003ECB68,003ECB68) -> r0=0
[postpoke] 0E80002C=003EE12C        (entry0+0x14 = Menue-Objekt)
[postcall2] 0x002975AC(0E800018) -> r0=0
[dumpmem2] 0E800000: 00000004 0 0 0 0 0 003CAC3C 0 0000FFFF ...
```

Der Entry-Draw laeuft mit gesetztem Kind (Vtable 0x3C6FE4, +8=0x11DB84),
liefert aber keine PICA-Paare. `ops=4` weiter aus dem Descriptor.


### Echter Sprite-Deskriptor als Kind gesetzt; 0x2812EC/0x27B34C laufen

`HLE_POSTCALL`-Sequenz:

```
[postcall2] 0x00297618(003ECB68,003ECB68) -> r0=0
[postpoke] 0E80002C=0CFE7000        (entry0+0x14 = HLE-Sprite-Deskriptor)
[postcall2] 0x002812EC(0,0CFE7000) -> r0=0
[tex] 0x132370(tex=0CFE7100) -> r0=1
[tex] 0x27B34C bind -> r0=0
```

Der echte Sprite-Emitter (`0x2812EC -> 0x27B34C`, Textur 0xCFE7100)
laeuft also auf dem Kind-Objekt, liefert aber weiterhin keine
`(value,header)`-Paare; `0x0E800000` bleibt Count/Descriptor,
`ops=4` stammt aus gsp_engine_desc.


### guest-watch: genaue Schreiber der Engine-Liste

`--range 0x0E800000,0x0E801000`:

```
write 0x0E800000 0x00000004  0x00143F60  fn_00143F60            (Count)
write 0x0E800004 0x00000000  0x0012FCB0  fn_0012FC24+0x8C        (Emitter nullt)
write 0x0E800000 0x003C4F4C  0x00288890  fn_00288890            (Builder)
write 0x0E800000 0x003CAC3C  0x00288890  fn_00288890            (Entry-Vtable)
write 0x0E800008 0xFFFFFFFF  0x00288890  fn_00288890            (Maske)
```

Also: Der Emitter `0x12FC24` schreibt nur Count + Nullwoerter; der
Builder `0x297618` schreibt Descriptor-Eintraege (Vtable/Maske).
`0x269248` ist ein **Listen-Insert** (`[ctx+0x170]/[ctx+0x16C]`),
kein PICA-Schreiber. Die `(value,header)`-Paare entstehen erst beim
Ausfuehren dieser Engine-Node-Liste - dieser GPU-Pass fehlt im Port.


beim Lauf haben **0 Reads** auf die eingefuegte Node-Liste (`ctx+0x16C
= 0x42B918`). Der Emitter baut/verlinkt die Liste, aber in unserem Lauf
konsumiert sie **niemand** - genau deshalb entstehen keine
`(value,header)`-Paare. Der Consumer-Pass fehlt.


### Consumer gefunden: 0x13A4B0 (Flush) - blockiert im Port

- `0x13A4B0` wird nur von `0x2BC800` gerufen.
- Dort: `r0 = fn_002758E0()` -> **0x42BB10**, dann `bl 0x13A4B0`.
- `guest-watch --pc 0x13A4B0` im normalen Lauf: **0 Treffer** (Pass laeuft nie).
- Direktaufruf `0x13A4B0(0x42BB10)` per HLE_POSTCALL: **kein Rueckgabewert**
  (haengt/blockiert), waehrend der Rest weiterlaeuft.

`0x13A4B0` ist also der gesuchte Node-/Draw-Flush (walkt Listen +0x1D4/+0x1E0 ,
ruft `fn_00275994` und `0x13F688(ctx)`), aber er blockiert im Port - vermutlich an
Lock/Resource-Zustand.


### Flush-Block eingegrenzt: 0x13F688 laeuft, 0x13A4B0 blockiert davor

- `0x13F688(0x42B7AC)` direkt: `r0=0` - Pool-Loop terminiert also.
- `0x13A4B0(0x42BB10)` blockiert weiterhin (kein Rueckgabewert).
=> Der Block liegt in `0x13A4B0` **vor** `0x13F688`, also im List-Walk
`[r4+0x1D4]`/`[r4+0x1E0]` (vtable `+8`/`+0xC`) von `0x42BB10` bzw. am
Recursive-Lock `0x42BB10+0x1C4` (`0x287A28`).


### Flush-Consumer 0x13A4B0 entblockt

Nach Belegen von Lock `0x42BB10+0x1C4` (Owner/Count=0) und leerer Listen
`+0x1D4`/`+0x1E0` (Selbstzeiger):

```
[postcall2] 0x0013A4B0(0042BB10,00000000) -> r0=0042BCD4 ab=0
```

Der Pass laeuft jetzt durch. Er konsumiert den Pool (`ctx+0xB4` via
`0x13F688`) - die separat gemessene Liste `ctx+0x16C` (aus `0x269248`)
hat weiterhin 0 Reads, ist also eine andere (Pending-)Liste.


### Flush laeuft, aber Pool ist leer

`0x13A4B0` kehrt jetzt zurueck (`r0=0x42BCD4`). Der Flush-Dispatch
`0x142BD0` springt per Tabelle auf `[node+4]` (Typ 0..0x3C) und ruft je
Knoten `[node+0x10]->[[vt]+0x14]` (Drawable) - z. B. Handler `0x142D34`.

Gemessen: Pool-Count `0x42B888` (`ctx+0xB4+0x28`) ist **0** ->
`0x13F688` poppt nichts, also keine Knoten/Drawables -> keine PICA-Paare.

Der Emitter legt seine Knoten in die **Pending-Liste ctx+0x16C**
(0 Reads), nicht in den Pool, den der Flush leert. Diese beiden Listen
sind im Port nicht verbunden.


### Verbindungsversuch Pending->Pool

Versuch: `0x12493C` mit Container `r1=0x42B7AC` und **ohne** erzwungenen
Allokator (`284AA8` real). Ergebnis: `0x13A4B0` blockiert wieder
(kein Rueckgabewert), `ops` bleibt 4. Mit der stabilen Konfiguration
(Container 0x3EC028, `284AA8=0E800000`) kehrt der Flush zurueck
(`r0=0x42BCD4`), Pool-Count bleibt aber 0.

Fazit: Der Allokator/Container muss die Pool-Basis `ctx+0xB4`
(Base/Maske/Count aus `0x126D28`) treffen; das ist noch nicht der Fall.


### Pool-Push gemessen: nur der Initializer schreibt den Count

- `guest-watch --pc 0x2669C0`: **0 Treffer** (Push-Pfad laeuft nie).
- `guest-watch --write 0x42B888` (`ctx+0xB4+0x28`): **1 Schreibzugriff**,
  Wert 0, `pc=0x26E180`, `lr=0x126D78` -> nur `0x126D28`-Init.
- Auch mit Builder-Container `0x42B860` (`ctx+0xB4`) bleibt der Count 0;
  `0x284AA8` ist ein Arena-Allokator und pusht nicht in den Pool.

Also: Der Pool wird im Port **von niemandem gefuellt**. Deshalb findet
der (jetzt laufende) Flush `0x13A4B0`/`0x13F688` nichts zu zeichnen.


### Pool-Push + Flush-Dispatch funktionieren jetzt

- Push: `0x2669C0(0x42B7AC, 0x0D600000)` -> `r0=0x42B7DC`, Pool-Count
  steigt (Write 0x42B888: 0 -> 1 via `0x26556C` aus `0x2669C0`).
- Flush `0x13A4B0(0x42BB10)` -> `r0=0x42BCD4`.
- Dispatch: `guest-watch --pc 0x142BD0` **1 Treffer** mit
  `r0=0x0D600000` (Aufrufer `0x13F688+0x2C`).

Damit ist die Kette **Push -> Pop -> 0x142BD0-Dispatch** erstmals
geschlossen. Der Node ist noch typ-/drawable-los (Felder 0), daher
entstehen noch keine Zeichendaten.


### HLE_POKE-Bugfix + typisierter Node

- `HLE_POKE` hatte nur **16 Slots**; spaetere Pokes wurden still verworfen.
  Jetzt 64 Slots -> alle Pokes wirken:
  ```
  [dumpmem2] 0D600000: 00000000 00000006 00000000 00000000 0CFE7000 ...
  [dumpmem2] 0CFE7000: 0D610040 0CFE7100 ...
  ```
  Node hat Typ 6 und Drawable `0x0CFE7000` (Vtable-Zeiger -> 0x0D610040).
- Dispatch `0x142BD0` wird mit `r0=0x0D600000` betreten; der Typ-6-Handler
  `0x142D34` bzw. der Stub `0x0D620000` werden per PC-Watch nicht
  beobachtet (indirekte Sprungziele werden vom Hook offenbar nicht
  gesampled).


### Puffer-Bugs behoben; Typ-6-Handler weiter nicht beobachtet

- `HLE_POKE`: 16 -> 64 Slots (spaetere Pokes wurden still verworfen).
- `HLE_POSTCALL`/`HLE_PRECALL`: Puffer 320/128 -> 1024/512 (Sequenz wurde
  abgeschnitten; dadurch fehlten Tail-Aufrufe).
- Stub-Bytes und Vtable verifiziert:
  ```
  [dumpmem2] 0D620000: E59F1010 E59F2010 E5821000 E12FFF1E ... CAFEBABE 0D630000
  [dumpmem2] 0D610050: 00000000 0D620000   (V+0x14 = Stub)
  ```
  Der Stub schreibt bei direkter Ausfuehrung `CAFEBABE` nach `0x0D630000`
  (45 Treffer). Wird er nur ueber den Handler gerufen, bleibt die Marke 0.

=> `0x142BD0` wird mit `r0=0x0D600000` betreten, aber der Typ-6-Pfad
`0x142D34 -> Stub` wird (per Marke) nicht ausgefuehrt. Die Typ-Auswahl
im Dispatcher ist damit der naechste Punkt.


### Typ-6-Handler laeuft, ruft aber den Stub nicht

Direktaufruf (minimale POSTCALL-Sequenz):

```
[postcall2] 0x00142D34(0D600000,00000000) -> r0=00000000 ab=0
[dumpmem2] 0CFE7000: 0D610040 0CFE7100     (Drawable -> Vtable)
```

Der Handler wird also ausgefuehrt, aber der Stub (`0x0D620000`, Marker
`CAFEBABE`) wird **nicht** erreicht (Marke bleibt 0). Der Fehler liegt
somit in der Indirektion des Handlers
(`[node+0x10] -> [[+0] ]+0x14`), nicht im Dispatcher.


### Vtable-Layout verifiziert, Stub trotzdem nicht erreicht

`[dumpmem2] 0D610040: 0 0 0 0 0 0D620000 ...` -> Index 5 = `+0x14` ist
`0x0D620000` (korrekt). Auch nach Verlegen von Stub (`0x0D7F0000`) und
Vtable (`0x0D7F2000`) und Marke (`0x0D7F1000`) bleibt die Marke 0.

Der Handler `0x142D34` laeuft (`r0=0`), aber die Indirektion
`[node+0x10] -> [[+0] ]+0x14 -> blx` erreicht den Stub nicht. Naechster
Verdacht: Laufzeit-Code/Daten weichen von `code_decomp.bin` ab (wie
schon beim `+2`-Vtable-Wert).


### ROOT CAUSE: Recompiler behandelt `ldr pc,...` nicht als Sprung

`build/gen_boot.c` fuer `fn_00142BD0`:

```
c->r[15] = arm_ld(c, table+off, 4, 0);
return;      <-- kehrt zum C-Aufrufer zurueck, springt NICHT zum Handler
```

Damit lief die Jump-Table-Dispatch (`0x142BD0`) nie in die Handler
(`0x142D34` usw.) - die Ursache fuer alle bisherigen "Handler laeuft
nicht"-Messungen. Fix in `tools/recomp/emit.py`:

```python
if load and rd == 15:
    lines.append("arm_call_guest(c, c->r[15]);")
    lines.append("return;")
```

Regeneriert: `python tools/build_boot.py --regen --port`.
Tests: `mvp_selftest` 13/13, `selftest_tools` 12/12 (inkl. `ldm_stm`,
`thumb_schleife`). Kanonisch unveraendert (TOP 65962, BOTTOM 26091).


### Nach Recompiler-Fix: Dispatch betritt Handler weiterhin nicht

```
guest-watch --pc 0x142BD0: 1 Treffer, r0=0x0D600000 (aus 0x13F688+0x2C)
[dumpmem2] 0D600000: 00000000 00000006 00000000 00000000 0CFE7000 ...
guest-watch --pc 0x142D34: 0 Treffer
```

Node/Dispatch korrekt (Typ 6, Drawable 0x0CFE7000), Flush kehrt zurueck
(`r0=0x42BCD4`). Der generierte `ldr pc`-Dispatch ruft jetzt
`arm_call_guest(c, c->r[15])`, aber der Handler wird nicht gesampelt.

Naechster Verdacht: `c->r[15]` ist im generierten Code nicht 0x142D34
(Tabelle/Offset oder `arm_ld`-Wert), oder `arm_call_guest` findet das
Ziel nicht (g_entries).


### Jump-Table-Dispatch funktioniert jetzt (grosse Strukturkorrektur)

Nach Recompiler-Fix + Logging (`HLE_LDRPC_LOG`):

```
[ldrpc] -> 00142D34
guest-watch --pc 0x142D34: 1 Treffer  r0=0x00000006  lr=0x13F688+0x2C
guest-watch --pc 0x2812EC: 2 Treffer, davon
   r0=0x0CFE7000  lr=0x142D5C  (aus dem Typ-6-Handler!)
```

Damit ist die Kette **Flush 0x13A4B0 -> Pop 0x13F688 -> Dispatch
0x142BD0 -> Handler 0x142D34 -> Drawable 0x2812EC(0xCFE7000)** erstmals
vollstaendig ausgefuehrt. Tests: 13/13 und 12/12. Kanonisch unveraendert
(65962/26091), weil der kanonische Pfad diese Tabellen nicht nutzt.


### Drawable-Versuch: 0x1105F8 als Ziel

Testweise `V+0x14 = 0x1105F8` (2D-Emit, r0=Puffer). Watch auf 0xCFE7000 zeigt
Schreibzugriffe von Engine-Funktionen (`0x1276A8`, `0x28EEF8` via `0x12173C`),
aber keine messbare PICA-Ausgabe. Profil wieder auf `0x2812EC` zurueckgestellt.
Der Drawable-Aufruf ist damit ausfuehrbar; es fehlt der **echte Drawable-Typ**
mit passenden Feldern/Argumenten.


### Echter Drawable: Handler ruft Engine-Draw 0x11DB84

Drawable-Vtable `0x0D7F2000` mit `+0x14 = 0x0011DB84` (echte Menue-Draw-
Methode), `node+0x10 = 0x0D7F2000`:

```
guest-watch --pc 0x11DB84 : 1 Treffer
  pc 0x0011DB84  r0=0x0D7F2000  lr=0x142D34+0x28
```

Damit ruft der Typ-6-Handler `0x142D34` jetzt eine **echte Engine-Draw-
Methode** mit `r0`=Drawable auf (statt nur eines HLE-Stubs). Das ist die
produktive Form des Drawable-Aufrufs.


### Echter Menue-Drawable: Builder/Loader laufen mit r0=0x0D800000

Menue-Objekt `0x3EE12C` nach `0x0D800000` kopiert, Vtable auf
`0x0D7F2000` (`+0x14=0x0011DB84`), Gate `+0x40=1` gesetzt, als
Drawable an `node+0x10` gebunden:

```
guest-watch --pc 0x2800C8: 218+ Treffer, r0=0x0D800000, lr=0x277004+0xD4
guest-watch --pc 0x28D424: 251+ Treffer, r0=0x0D800000, lr=0x277004+0xE8
guest-watch --pc 0x277004: 1 Treffer,  r0=0x0D800000, lr=0x11DB84+0xC
```

Damit baut die **Engine** den Menue-Draw mit dem echten Menue-Objekt
(Builder 0x2800C8 + Loader 0x28D424), ausgeloest ueber
Handler 0x142D34 -> 0x11DB84 -> 0x277004.


### Ursache der leeren Liste gefunden: Allokator-Aliasing

`guest-watch --write 0x0E800000` (mit erzwungenem `284AA8=0x0E800000`):

```
0x0E800000 <- 0x00000004   from 0x143F60   (Count)
0x0E800000 <- 0x003C4F4C   from 0x288890   (Builder)
0x0E800000 <- 0x003CAC3C   from 0x288890   (Entry)
0x0E800000 <- 0x00000000   from 0x279E50   (Node-Fill, 4x)
```

Der **Node-Fill `0x279E50`** (aus `0x2895D0`) ueberschreibt die Liste, weil
der erzwungene Arena-Allokator `0x284AA8` **denselben Puffer** liefert.
Deshalb bleibt `0x0E800000` bei Count/Descriptor/Nullen.

Mit echtem Allokator (ohne `284AA8`-Zwang) wird `0x0E800000` gar nicht
mehr geschrieben (Emitter/Chain haengt wieder) - daher Zwang zurueckgestellt.


### Bump-Allokator korrigiert (getrennte Puffer), Emitter laeuft dann aber nicht

`HLE_ALLOC_BUMP` wird jetzt getroffen und nutzt feste Stride:

```
[bump] 0x284AA8 -> 0E900000 sz=1000
```

Damit sind Liste (`0x0E900000`) und Node-Pool getrennt (kein Aliasing).
In dieser Konfiguration wird `0x0E900000` jedoch **nie geschrieben**, weil
`0x12FC24` dann nicht mehr laeuft (Chain haengt vorher). Stabile
Konfiguration (Zwang `284AA8=0x0E800000`) wiederhergestellt.


### Bump-Allokator: Liste persistiert jetzt (kein Aliasing)

Mit `HLE_ALLOC_BUMP=0x0E900000` (Stride 0x1000) und ohne `284AA8`-Zwang:

```
[bump] 0x284AA8 -> 0E900000 sz=1000
[emitin] r2=0E902000 r3=00000014 dsc[3EE678]=00000000 ...
guest-watch --write 0x0E902000: 1 x Wert 0x00000004 (0x143F60 = Count)
[dumpmem2] 0E902000: 00000004 00000000 ...   (bleibt erhalten!)
```

Der Emitter laeuft (`0x12FC24` 1x, `0x12FD30` 114x, `0x139914` 1x) und
die Liste wird **nicht mehr vom Node-Fill ueberschrieben**. Sie enthaelt
allerdings nur den Count (4), keine `(value,header)`-Paare - die Eintraege
liegen in den separaten Node-Puffern.


### Node-/Item-Puffer gemessen: 0x0E905000 = Vtable 0x3C4F4C

`guest-watch --range 0x0E900000,0x0E910000`:

```
0x0E902000 <- 0x00000004  (Count, 0x143F60)
0x0E902004.. <- 00000000  (Emitter 0x12FCB0)
0x0E905000 <- 0x003C4F4C  (Builder 0x288890 -> 0x297618)
```

Statische Vtable `0x3C4F4C`:

```
+00=00285BEC  +08=0028EF3C (Unlink!)  +10=003C454C  +1C=001BD720
```

Also: Der Builder legt Draw-Items mit Vtable `0x3C4F4C` ab; deren
`+8`-Methode ist erneut der Unlink-Pass `0x28EF3C`. Die eigentliche
GPU-Uebersetzung (value,header) liegt hinter `+0x00`/`+0x10`.


### Vtable 0x3C4F4C aufgeloest: +0 ist Destruktor/Unlink, kein Emitter

`0x285BEC` (Vtable+0x00): setzt Vtable, laeuft Listen ab und ruft
`0x27A3B4` (der Unlink-Pass) - also **Lifecycle/Unlink**, kein
GPU-Emitter. Die Draw-Items selbst sind damit reine Objekte; die
(value,header)-Erzeugung liegt in einem anderen Pass.


### Architektur-Befund: Engine schreibt weder Kommandoliste noch GSP-Queue

`guest-watch` im Chain-Lauf:

```
--write 0x0CFE6000: NIE GESCHRIEBEN
--write 0x10002800: NIE GESCHRIEBEN
```

Die HLE fuellt `0x0CFE6000` (2D-Stub) und den GSP-Queue-Puffer selbst
(Host-Code); der **Gast-Code** ruft die gsp-Wrapper (`0x1196AC` /
`0x120DA0`) im Port nie auf. Deshalb gibt es keinen engine-eigenen
Submit und nur den Descriptor-Fallback (qops=4).

Abhilfe: die Engine-gsp-Wrapper explizit mit der gebauten Liste
aufrufen (SetCommandList + Trigger) und pruefen, ob `0x10002800`
dann vom Gast geschrieben wird.


### Engine-gsp-Wrapper 0x1196AC blockiert im Port

Minimaler Aufruf `0x1196AC(0,0,0,0)` per HLE_POSTCALL:

```
[postcall2] 0x001196AC(00000000,00000000)
(kein -> r0 ...)   -> blockiert/kehrt nicht zurueck
guest-watch --write 0x10002800: NIE GESCHRIEBEN
```

Der Wrapper laeuft in einen GSP-/Service-Wartepfad (0x120CFC/svc) und
kehrt im Port nicht zurueck - daher kein Gast-Schreibzugriff auf den
CommandBuffer.


### Block in 0x1196AC lokalisiert: GSP-SVC-Pfad

`0x1196AC` -> `0x120CFC` -> `0x275B24`:

```
00275B24 push ... ; mrc p15,0,r4,c13,c0,3 ; str [r4,#0x80]! cmd ; svc #0x32
```

Der Wrapper endet also in einem **GSP-IPC-SVC (0x32)**. Beim Direktaufruf
aus dem HLE-Kontext kehrt dieser Pfad nicht zurueck (IPC-/Reply-Wartepfad).


### svc-0x32-Pfad gemessen: Wrapper laeuft in Trigger-Schleife

`HLE_SVC32_LOG` beim Direktaufruf von `0x1196AC`:

```
[svc32] #1..#7  (u.a. #7 lr=0x2877FC blk=0x275AE0)
```

Danach Log-Tail:

```
ipc gsp::Gpu  cmd=0x000C   (TriggerCmdReqQueue)  (wiederholt)
svc 0x32 r0=0x00000002 r1=0
[sp] 0x0012FAA4 / 0x0026BF8C / 0x00101744 SP-Korrekturen
```

Der Wrapper kommt also bis zum GSP-IPC und laeuft dann in einer
**Trigger- (0x000C) Warteschleife** mit SP-Korrekturen im Submit-Pfad -
er kehrt nicht zurueck und schreibt `0x10002800` nicht.


### SP-Korrekturen: drei Call-Sites im Submit-/Trigger-Pfad

`arm_restore_sp` (runtime/arm_cpu.c:730) meldet unbalancierte Callees:

```
[sp] 0x0012FAA4: SP 0CF09F70 -> 0CF09F68
[sp] 0x0026BF8C: SP ... (3x)
[sp] 0x00101744: SP 0CF099F8 -> 0CF09A48
```

Die Runtime korrigiert SP zwangsweise (ABI-Garantie), aber der Callee
hat seinen Stack nicht sauber restauriert - ein Recompiler-/EPILOG-
Problem. Beim Aufruf von `0x1196AC` bleibt die Ausfuehrung danach in
diesem Pfad (kein Rueckgabewert, `0x10002800` ungeschrieben).


### Ursache der SP-Korrekturen gefunden: In-Funktions-Jump-Tabellen

Die ldr-pc-Ziele in `0x12FAA4` sind **innerhalb derselben Funktion**
(0x12FAD4/0x12FADC/0x12FAE4, 0x12FB30/38/40/44, 0x12FB98/BA0/BA8) - keine
eigenen fn_-Eintraege. Unser `arm_call_guest(c, target)` findet sie
nicht und kehrt sofort zurueck, **ohne** das spaetere `pop {r4,r5}` der
Funktion auszufuehren -> SP bleibt 8 Byte zu tief (genau die gemessene
Differenz). `arm_restore_sp` korrigiert das nur nachtraeglich.

Konsequenz: `0x12FAA4` (Config-Setter) liefert falsche Werte/Stack, und
der Submit-Pfad verliert den Faden (0x1196AC kehrt nicht zurueck).

Richtiger Fix: `ldr pc` fuer **lokale Label-Ziele** als `goto
L_<fn>_<addr>` emittieren (switch ueber die Tabellenwerte), statt
`arm_call_guest`.


### Lokaler ldr-pc-Switch implementiert (Block-Labels)

`emit.py`: `Emitter.block_labels`; fuer `ldr pc` wird jetzt ein `switch
(target) { case <block>: goto L_<fn>_<addr>; default: arm_call_guest }`
emittiert. `cli.py` setzt `em.block_labels = set(fn.blocks)`.

Ergebnis: Funktionen, deren Tabellenziele **Blockanfaenge** sind, springen
jetzt lokal (Epilog bleibt erhalten). Fuer `0x12FAA4` sind die Ziele
allerdings (noch) keine Blockanfaenge -> weiterhin `arm_call_guest` und
SP -8. Naechster Schritt: Scanner muss Tabellenziele als Blocks
splitten.

Tests nach Regen: 13/13, 12/12.


### Praezise Ursache: ldr-pc-Ziele wurden als *eigene Funktionen* gesplittet

`fn_0012FAA4`-Switch enthaelt nur `case 0x0012FAA4`; die Tabellenziele
(0x12FAD4, 0x12FADC, ...) sind bereits eigene `fn_0012FAD4`-Fragmente
(ohne Prolog/Epilog). `arm_call_guest` ruft so ein Fragment -> dessen
`pop {r4,r5}` fehlt, SP bleibt -8 (gemessene Korrektur).

Fix-Bedarf: diese Fragmente nicht als eigene Funktionen fuehren, sondern
als Blocks der umschliessenden Funktion (Merge), damit der lokale
Switch/goto greift.


### Merge-Versuch: Kriterium zu breit (1387 Fragmente)

`_merge_ldrpc_fragments` (ldr-pc-Ziele, die eigene fn_ sind, ohne Prolog)
merge-te **1387** Fragmente -> doppelte Labels, Build-Fehler. Kriterium
muss enger sein: nur mergen, wenn das Ziel **innerhalb** eines schon
existierenden Blocks der Elternfunktion liegt (echtes In-Funktions-Label),
nicht bei echten Quer-Funktions-Eintraegen. Merge-Aufruf wieder entfernt,
Build/Tests gruen (13/13, 12/12).


### Enger ldr-pc-Merge aktiv (Build gruen) + neuer Blocker 0x141C38

- `_merge_ldrpc_fragments` mergt nur **direkt angrenzende** Fragmente
  (Ende des letzten Blocks == Zieladresse), dedupliziert Blocks.
- Build gruen, Tests 13/13/12/12. Die alte SP-Korrektur bei `0x12FAA4`
  ist **verschwunden** (jetzt 0x28EA8C/0x1385D0/0x28E838).
- `0x1196AC` haengt nicht mehr, kehrt mit `ab=1` zurueck, bricht aber an
  `arm: not implemented: empty (pc 0x00141C38)` ab - dort fehlt kompilierter
  Code (weiteres ldr-pc-/Fragment-Loch, Seed 00141C38 half nicht).
- `0x10002800` weiterhin ungeschrieben.


### Globaler ldr-pc-Vor-Pass + Dedupe (Build gruen)

- `_all_split_ldrpc`: splittet vor der Funktionsliste an ldr-pc-Zielen in
  der Elternfunktion und entfernt leere Duplikate.
- Block-Dedupe nach Merge/Split (keine doppelten Labels mehr).
- Merge: 1979 Fragmente, Build gruen, Tests 13/13 + 12/12.
- `0x1196AC` kehrt zurueck (ab=1), bricht aber weiterhin an
  `arm: not implemented: empty (pc 0x00141C38)` ab. Die Adresse ist eine
  gueltige Leaf-Funktion, wird aber als leere Funktion generiert.


### 0x141C38-Abort weg (Fill ohne Merge)

Ohne kontiguierlichen Merge, aber mit `_fill_empty_fns` (+ Functions fuer
needed), hat `0x141C38` einen echten Block; der Abort
`empty (pc 0x00141C38)` tritt nicht mehr auf.

`0x1196AC` kommt weiter, endet aber in indirekten Zielen:
```
arm: unbekanntes call-Ziel 0xFF8001FF (von Block 0x0028AB04)
arm: unbekanntes call-Ziel 0x00000000 (von Block 0x0028BE0C)
```


### Indirekte Fehlziele: Vtable-Slot +0x78 ist 0/0xFF8001FF

Beide Meldungen stammen aus `blx r1`, wobei

```
0028BE0C: ldr r1,[r0] ; ldr r1,[r1,#0x78] ; blx r1
0028AB04: (analog, Vtable-Slot +0x78)
```

Der Slot `+0x78` des uebergebenen Objekts ist **0** (nicht konstruiert)
bzw. `0xFF8001FF` (Fehlercode) -> `arm_call_guest` meldet unbekanntes Ziel.
Damit fehlt erneut eine echte Objekt-/Vtable-Bindung, nicht der Code.


### Recompiler-Einstieg gefixt + echter GSP-Submit-Ring (2026-11-23)

Zwei konkrete Fehler gefunden und behoben:

1. **Falscher Funktions-Einstieg.** `generate()` emittierte
   `goto fn.blocks[0].addr`. Bei Funktionen, deren erster Block ein
   Quer-/Tail-Ziel ist, sprang der Einstieg damit an die falsche Adresse.
   Beispiel `fn_00381CE4` (Scene-Konstruktor):
   vorher `goto L_00381CE4_00207A10;`, jetzt
   `goto L_00381CE4_00381CE4;`. Folge: der Konstruktor lief vorher leer,
   `[0x3EC028]` blieb 0. Jetzt:
   `[engine] nach Konstruktor [scene]=003CA494 ab=0 W: 003CA494 ...`.
   Zaehler im generierten Code: 17521 Funktionen, 0 mit falschem Einstieg.

2. **SubmitCmdList -> Shared-Memory-Ring.** `gsp_command` Fall 0x0006
   schreibt den Eintrag jetzt so, wie es die GSP-Firmware tut:
   `hdr=1<<8` bei 0x10002800, `id=1`, `addr`, `size` bei 0x10002820..28.
   Die synthetischen HLE_POKEs fuer 0x10002800/0x10002820..28 sind aus
   `tools/profiles/chain.json` entfernt. Damit kommt `qops=1` aus der vom
   Gast per SetCommandList uebergebenen Engine-Liste (0x0CFE6000, 0x24 B)
   und aus `gpu_run_command_list` (ops=1), nicht aus `gsp_engine_desc`.

Belege (`python tools\run_chain_direct.py`, out/logs/chain_direct.txt):

```
[gsp] SetCommandList addr=0x0CFE6000 size=0x24 lr=001ED9C8
[gspq] tid=0 idx=0 id=1 list=0CFE6000 size=24 ops=1
[gsp] Trigger lr=001ED9C8 addr=0CFE6000 size=24 qops=1
```

Liste (`GPU_LIST_LOG=4`):

```
[gpulist] addr=0CFE6000 size=24 070FFFFF 00000007 00000000 00000000 000000C8 00000078 1E000000 00000190 000000F0
```

Bild (`out/shot_chain_direct.bmp`, beide Screens ueber den GSP-Pfad):

```
TOP    == out/menu_top.bin    : 0/96000  Pixel >60
BOTTOM == out/menu_bottom.bin : 0/76800  Pixel >60 (bei x-Offset 40 im 400er-Fenster)
gegen reports/menu_official*.bin: TOP 90281/90400, BOTTOM 26091/71360
```

Noch offen: die Pixel kommen weiterhin aus den vorbereiteten Assets
(0x1E000000/0x1E100000) und dem HLE-2D-Blit; die echte `.xq`-Menuekomposition
ueber FA-Reader `0x280744`/Builder `0x2800C8` ist noch nicht angesteuert
(0 Treffer), `0x28D424` laeuft nur ueber den HLE-Direktaufruf.
Tests: 13/13, 12/12.


### Feedback umgesetzt: generischer Obj-Hook + ehrlicher Gate-Test (2026-11-23)

- Neuer HLE_HOOK-Callback `obj` (auch via `tools\hotpatch.py`): druckt
  `r0..r3`, `lr`, `[r0]` und die Vtable-Slots `+0x00..0x1C` sowie
  `+0x70..0x8C`. Damit ist die frueher muehsam erratene +0x78-Frage ein
  Einzeiler (`--hook 28BE0C=obj`). Beispiel:
  `[hook] obj addr=0028D424 ... r0=003EE12C ... [r0]=00000000`.
- Einmal-Patcher entfernt (`tools\_patch_scan_ldrpc.py`,
  `_merge_fragments*.py`, `_fill_empty.py`); Aenderungen laufen direkt
  ueber `edit`.

**Ehrlicher Gate-Test** (`out/profiles/none.json`: HLE_RETFORCE und
HLE_POSTCALL komplett entfernt, Ring-Pokes schon vorher raus):

```
TOP / BOTTOM == out/menu_top.bin / out/menu_bottom.bin : 0/96000 + 0/76800
[gspq] tid=0 idx=0 id=1 list=0CFE6000 size=24 ops=1
[gsp] Trigger ... qops=1
```

Der Dual-Screen-Render ueber SetCommandList(0x0CFE6000)/Trigger haengt also
**nicht** an erzwungenen Gates oder POSTCALL-Pokes. Nur der alte
Engine-Submit `0x139914`/`0x13F2D4` verschwindet ohne HLE_RETFORCE
(0 Treffer), waehrend `qops=1` bleibt.

**Naechster echter Ressourcen-Blocker** (gleicher Lauf, 0 RETFORCE/POSTCALL):

```
[hook-final] 0x28D424 hits=26     (nur HLE_ENGINE_RES-Direktaufruf)
0x280744 (FA-Reader)  hits=0
0x2800C8 (Draw-Builder) hits=0
0x2919DC             hits=0
```

Damit ist belegt: der Loader wird aufgerufen, steigt aber nicht in den
FA-Reader/Builder ab; die Pixel kommen weiterhin aus den vorbereiteten
Assets. Tests: 13/13, 12/12.


### Draw-Builder erstmals erreichbar: echter Treiber 0x277004 statt Drain (2026-11-23)

Der FA-Reader-Agent hat belegt: `0x28D424` ist nur der Drain/Consumer;
der echte Treiber ist `fn_00277004` (ruft `0x2770D4 bl 0x2800C8` und
`0x2770E8 bl 0x28D424`). `HLE_ENGINE_RES` rief bisher nur den Drain.

Gate-Messung mit dem generischen `obj`-Hook (`--hook 277004=obj`):

```
r0=003EE12C [r0]=00000000 obj+38: 00000000 00000000 00000000 00000000
obj+68: 00010000 0CFF0000 00000000 01000000 00000000
```

`[r0+0x40]` (=0x3EE16C) ist 0, deshalb `0x27708C beq 0x2770F8` und der
Builder wird uebersprungen. Ursache: `HLE_MENU_OBJ0=1` schrieb dort 0.
Mit `HLE_POKE=...,3EE16C=00000001` und `HLE_POSTCALL=277004,3EE12C,0,0,0`
(env-only, kein Regen/Build):

```
[hook-final] count addr=002800C8 hits=56     (vorher 0)
[hook-final] count addr=0028D424 hits=82
[gspq] tid=0 idx=0 id=1 list=0CFE6000 size=24 ops=1
[port] frames=59 pc=002896C4 units=0 dl=0/256 arr=0CFF0000
```

Damit laufen Draw-Builder und Drain erstmals gemeinsam ueber den echten
Treiber. Der FA-Reader `0x280744` bleibt 0: er haengt am separaten
Request-Pfad `0x2919DC -> 0x104E44 -> 0x1112C4 -> 0x280744` (bzw.
`0x1117A4` fuer den Menue-Request), nicht am Drain. Screenshot unveraendert
(Assets): 90281/90400 + 26091/71360 gegen reports/menu_official*.bin.


### Echter Menue-Request laeuft bis zur Engine-Draw-Methode (2026-11-23)

Profil: `tools/profiles/menu_request.json` (env-only, kein Regen).

**1. Request-Pfad mit korrekten Argumenten** (`HLE_POSTCALL=28F7BC,3EC028,0,0,0`):

```
[postcall2] 0x0028F7BC(003EC028,00000000)
[hook] count addr=0028A1BC hits=1
[hook] obj addr=001117A4 hits=1 r0=003EE12C r1=0CF09F18 r2=0CF09FD0 r3=00000000 lr=0028A234
           [r0]=00000000 obj+38: 00000000 00000000 00000001 00000000
[postcall2] -> r0=00000001 ab=0
```

`r0=0x3EE12C` = Scene+0x2104, `[menu+0x40]=1`; der Request erreicht den
Builder. Bisher trat dabei ein Null-Vtable-Call auf:

```
arm: unbekanntes call-Ziel 0x00000000 (von Block 0x002801D4)
```

Ursache: `0x2801D4` ruft `[[r4]]+0x8`; das Pool-Objekt `0x3EE12C` hatte
`[r0]=0`. Mit der echten Klassen-Vtable `0x3C6FE4` (+8=`0x11DB84`) und
`HLE_POOL_MENUOBJ=1`:

```
[hook-final] count addr=002800C8 hits=214
[hook-final] count addr=0011DB84 hits=23
[hook] obj addr=0011DB84 ... r0=003EE12C r2=0CFF1000 lr=00280208
       [r0]=003C6FE4 vt+00..1C: ... 0011DB84 ...
```

Damit ruft der Draw-Builder pro Node die **echte Engine-Draw-Methode**
`0x11DB84` (Vtable-Slot +8) auf; keine unbekannten Call-Ziele mehr.

**Noch offen:** FA-Reader `0x280744` bleibt 0 (eigener Pfad
`0x2919DC -> 0x104E44 -> 0x1112C4 -> 0x280744`), und die Pixel kommen
weiterhin aus der HLE-Liste (0x070FFFFF/0x0CFE6000):
TOP 90281/90400, BOTTOM 26091/71360 gegen reports/menu_official*.bin.
Tests: 13/13, 12/12.


### FA-Reader liest das Menue-.xq aus lt6_a.fa (2026-11-23)

Profil: `tools/profiles/menu_xq_read.json` (env-only).

Zweig `0x2919DC -> 0x104E44 -> 0x1112C4 -> 0x2808A8 -> 0x280744`
mit `HLE_POSTCALL=2919DC,3EC028,0,0CF0B200,0CF0B200` gestartet
(Pfadstring "seq/menu_logo.xq" in Gast-RAM, Handle-Wort 0x400008,
Tabelle 0x0CFE7400 -> 0x00400000, Gate `[0x3D9440]=0x32335158`).

Ergebnis (`out/logs/fareader5_run.txt`):

```
[postcall2] 0x002919DC(003EC028,00000000)
[faread] idx=0 pos=00458CB8 want=00000048 n=72 dst=0CF09D10
[faread] idx=0 pos=00458D00 want=00000004 n=4 dst=0CF08CC8
[faread] idx=0 pos=00458D04 want=00000004 n=4 dst=0CF08CC8
[faread] idx=0 pos=00458D08 want=00000004 n=4 dst=0CF08CC8
[faread] idx=0 pos=00458D0C want=00000004 n=4 dst=0CF08CC8
[postcall2] -> r0=00000001 ab=0
```

Der Reader liest also den XQ-Header (0x48 B) und die vier Chunk-Groessen
am Offset `0x458CB8`. Kontrolle am Dateiinhalt:

```
lt6_a.fa@458CB8[:16] = 58513332110008004c0013006900c800   ("XQ32"...)
out/fa_a/seq/menu_logo.xq[:16] = 58513332110008004c0013006900c800
identisch: True
```

**Noch erzwungen:** der Ressourcen-Open `0x268C80` (HLE_RETFORCE auf
0x0E500000) und der Archiv-Offset (`HLE_FA_FORCE_POS=458CB8`). Beides ist
der naechste echte Gate (Lookup "seq/menu_logo.xq" -> Archivoffset).
Pixel unveraendert (HLE-Liste): 90281/90400 + 26091/71360.


### Echter ARC0-Index-Lookup fuer den Menue-Pfad (2026-11-23)

`runtime/fa_index.c` parst den echten `.fa`-Index (LZSS + Huffman +
CRC32 wie `tools/xfsatool/arc.cpp`) und loest einen Pfad zu
`dataOffset + entry.offset` auf. `runtime/arm_trace.c` merkt sich den
zuletzt an `0x2808A8` uebergebenen Pfad (r1) und ruft den Lookup in
`hle_fa_read` auf, wenn `pos == 0` ist. Damit entfaellt
`HLE_FA_FORCE_POS`.

Verifikation des Index (`python tools\fa_index.py ... --verify`):

```
seq/menu_logo.xq   off=0x00458CB8 size=0xA58  magic=58513332
seq/menu_title.xq  off=0x00465D60 size=0x12E8 magic=58513332
```

Lauf mit `tools/profiles/menu_xq_index.json` (kein `HLE_FA_FORCE_POS`):

```
[faread] idx=0 pos=00458CB8 want=00000048 n=72 dst=0CF09D10
[faread] idx=0 pos=00458D00 want=00000004 n=4
[postcall2] -> r0=00000001 ab=0
```

Ohne den `0x268C80`-Force (HLE_RETFORCE entfernt) gibt es **keine**
`[faread]`-Zeile und `r0=0`; der CFile-Pool-Open ist damit der letzte
erzwungene Teil (kein Offset-Force mehr). Tests: 13/13, 12/12.


### CFile-Pool-Gate isoliert: Engine-Heap fehlt (2026-11-23)

`0x268C80` liest den Pool-Deskriptor `0x40F048` (Literal `0x268D70`).
Messung mit `HLE_DUMPMEM=40F048,10`:

```
[dumpmem2] 0040F048: 00000000 ... (16x)
```

Der Pool ist also im Teillauf komplett 0 (`[+0x26]` count=0) -> `0x268C80`
liefert 0, `0x2808A8` bricht ab, keine `[faread]`-Zeile.

Der Engine-Initialisierer ist `fn_0011A890` (einziger BL-Aufrufer
`0x10F8CC`). Manuell aufgerufen
(`HLE_POSTCALL=11A890,0CF0B300,0,0,0` mit count=16) laeuft er bis zur
Allokation:

```
[hook] regs addr=0027B49C hits=1 r0=00000754 ... lr=0011A8EC
```

`0x27B49C` = `0x289AC0(heap=0x408448, size, 2)` liefert 0, weil das
Heap-Objekt `0x408448` im Teillauf nicht initialisiert ist (BSS). Danach
bleibt `[0x40F048]` 0 und `0x268C80` weiter 0.

Damit ist die Kette exakt:
`0x2808A8 -> 0x268C80 -> Pool 0x40F048 -> fn_0011A890 -> 0x27B49C ->
Heap 0x408448`. Der letzte erzwungene Teil ist der Heap, nicht der Pfad.


### Engine-Heap-Groessen gesetzt: 0x289B9C bekommt echte Werte (2026-11-23)

`0x10FE9C`/`0x10FEAC` lesen `[0x3EC00C]`/`[0x3EC008]` und fuettern damit
`0x289B9C(heap=0x408448, ...)`. Diese Felder waren 0 (der Kernel/Port setzte
sie nie), deshalb blieb der Heap leer.

`hle_init` schreibt jetzt die vom Boot per ControlMemory gelieferten
Groessen (0x05700000 / 0x02900000). Beleg am Aufruf:

```
vorher : [hook] regs addr=00289B9C r0=00408448 r1=00000000 r2=00000000
jetzt  : [hook] regs addr=00289B9C r0=00408448 r1=05700000 r2=02900000
```

Der Heap selbst ist zum Zeitpunkt der CFile-Pool-Allokation aber weiterhin
leer (`mem@00408448` 16x 0), `0x11A890` liefert 0 und `0x268C80` bleibt 0.
Der naechste Gate liegt damit in `0x289B9C`/`0x289AC0` (Segment-Init), nicht
mehr in den Groessen. `[faread]` erscheint weiter nur mit dem
`0x268C80`-Force. Tests: 13/13, 12/12.


### Heap-Segment/Sub-Allocator laufen - CFile-Pool-Deskriptor ist ein anderer Init (2026-11-23)

Mit Basis/Groesse `0x08000000`/`0x05700000` (statt vertauscht) und
`HLE_POSTCALL=289B9C,408448,08000000,05700000,2;11A890,...;11B05C,...`:

```
[postcall2] 0x00289B9C(...) -> r0=00000001
[postcall2] 0x0011A890(...) -> r0=00000001
[postcall2] 0x0011B05C(...) -> r0=00000001
write 0x003D9320 <- 0x08000020   (echte Engine-Allokation)
```

Heap-Segment und die beiden Sub-Allocator-Init laufen also. `0x268C80`
liefert aber weiter 0: sein Deskriptor `0x40F048` ist zu diesem Zeitpunkt
noch leer (Dump 64 Woerter = 0). Die Hash-Writes bei `0x40F070..0x40F07C`
(`fn_002892DC`/`0x289290` aus `fn_0010395C`) kommen von einem **anderen**
Init, nicht von `0x11A890`/`0x11B05C`.

Damit ist der naechste Gate: den CFile-Pool-Deskriptor-Init
(`0x10395C -> 0x289290/0x2892DC`) ausfuehren bzw. seinen Count-Store
finden. Tests: 13/13, 12/12.


### CFile-Pool ist 0x40F048; Init fuellt ihn nicht (2026-11-23)

Neuer generischer Dump: `HLE_OBJADDR=<hex>` im `obj`-Hook druckt 32 Woerter
an einer festen Adresse. Damit ist der Pool am Aufruf von `0x268C80`
sichtbar:

```
[hook] obj addr=00268C80 ... @0040F048: 00000000 ... (32x 0)
```

Der Ring-Init wird aber mit genau diesem Deskriptor aufgerufen
(`0x284A48`):

```
[hook] regs addr=00284A48 r0=0040F048 r1=0E905000 r2=00000010 r3=00000010
```

`0x284A48` setzt nur die Indizes zurueck; Array und Stride an
`[0x40F048]`/`[+4]` muessen vorher gesetzt sein. Mit
`HLE_POKE=40F048=0E905000,40F04C=00000004` steht der Ring zwar, aber
`[+0x26]` (Count) bleibt 0 - der Fuell-Loop `0x11ACBC` schreibt ihn nicht.

Damit ist der naechste Gate exakt: der Fuell-Loop/Statik-Ctor, der
`0x40F048+0x00/+0x04/+0x26` setzt. Heap/Allokation sind bereits ok
(`write 0x3D9320 <- 0x08000020`). Tests: 13/13, 12/12.


### Menue-.xq ohne erzwungenen CFile-Open gelesen (2026-11-23)

Der CFile-Pool-Deskriptor `0x40F048` wird jetzt ueber seine echte
Ringstruktur gefuellt: freier Ring `0x40F060` (Array `0x0E905000`,
Stride 4, Start/Ende 0, Count 16) mit einem CFile-Zeiger, dessen
`+0x10`-Open auf `0x10F8C4` (liefert 1) zeigt. Danach liefern
`0x268C80`/`0x2808A8` ohne `HLE_RETFORCE` gueltige CFiles.

Beleg (Profil `tools/profiles/menu_xq_noforce.json`;
`268C80` **nicht** in HLE_RETFORCE, `HLE_FA_FORCE_POS` nicht gesetzt):

```
[postcall2] 0x002919DC(003EC028,00000000)
[faread] idx=0 pos=00458CB8 want=00000048 n=72 dst=0CF09D10
[faread] idx=0 pos=00458D00 want=00000004 n=4
[faread] idx=0 pos=00458D04 want=00000004 n=4
[hook-final] count addr=00268C80 hits=1
[hook-final] count addr=002808A8 hits=1
```

Der Offset kommt aus dem echten ARC0-Index (`fa_index.c`), der Datei-Read
aus `lt6_a.fa`. Pixel unveraendert (HLE-Liste): 90281/90400 + 26091/71360.
Tests: 13/13, 12/12.


### CFile-Pool wird jetzt vom Engine-Fuell-Loop befuellt (2026-11-23)

Ursache des uebersprungenen Loops: `0x284A48` bricht mit 0 ab, wenn der
Ring-*Stride* (`[ring+4]`) 0 ist. Der Stride kommt aus dem Statik-Ctor.
Mit den drei Strides (`0x40F04C/0x40F058/0x40F064 = 4`) laeuft der echte
Loop:

```
[blk] 0011ABA0 ... -> 0011ACBC -> 0011ACD0 -> 0011AD2C -> (16x)
[blk] 0011AD2C r0=0E907000/0E907004/... r4=0040F060
```

Pool danach (OBJADDR-Dump am 0x268C80-Aufruf):
```
@0040F048: 0E905000 00000004 00100010 0E906000 00000004 00100000
            0E907000 00000004 00100010 00100000 ...
```
Count (`+0x26`) = 0x10, freier Ring-Array = `0x0E907000`; die Objekte
haben die echte Engine-Vtable `0x003C9B18` (Dump @0E905000).

`0x268C80`/`0x2808A8` rufen damit den **echten CFile-Open** auf:

```
[hook] regs addr=0032AF28 r0=0E905000 r1=0CF09E68 lr=002808EC
```

Der Open scheitert noch in `0x287D10 -> 0x26E95C` (Pfadformat/FS-State),
daher noch keine `[faread]` in diesem Lauf. Der synthetische CFile-Slot
ist weg; nur die drei Ring-Strides werden noch gepokt. Tests: 13/13, 12/12.


### Engine-Liste an 0x0CFE6000 submittet, HLE-Assetpfad aus (2026-11-23)

Neu: `HLE_2D_NO_BLIT=1` ueberspringt sowohl den Asset-memcpy
(0x1E000000/0x1E100000) als auch die HLE-0x070FFFFF-Liste+Submit in
`hle_2d_stub`. Zusaetzlich faellt die GSP-Route fuer 0x0CFE6000 jetzt
auf den PICA-Parser zurueck, wenn der Legacy-Blit-Parser 0 Ops liefert
(gsp_process_cmdqueue, Engine-Layout und 0x000C).

Lauf `tools/profiles/engine_list_cfe.json` (kein HLE_RETFORCE):

```
[gspq-engine] tid=0 n=1 list=0CFE6000 size=1000 ops=1
[gsp] Trigger lr=0012FE1C addr=00000000 size=0 qops=1
[hook-final] count addr=0012FD30 hits=19
[hook-final] count addr=00139914 hits=19
[hook-final] count addr=0013F2D4 hits=19
Screenshot: out/shot_engine_cfe4.bmp
```

Kein `[engine] Menue-Assets geladen` mehr; die Pixel sind flach
(96,64,32) und **nicht** mehr die Assets:
`vs out/menu_top.bin diff>60 95368/96000`.
Gegen `reports/menu_official.bin`: TOP 89358/90400, BOTTOM 71353/71360.

Ehrlich: Listeninhalt an 0x0CFE6000 ist noch der PICA-Teststrom aus
HLE_POKE; die Engine-Geometrie (Emitter 0x12FC24/C-Tabelle) fehlt noch.
Tests: 13/13, 12/12.


### Engine emittiert die Liste selbst an 0x0CFE6000 (2026-11-23)

Ohne Listen-Pokes, nur mit Descriptor (`0x0EA00000` -> C-Tabelle count=1),
schreibt `0x13F2D4` ihre Knotenliste selbst nach 0x0CFE6000:

```
[dumpmem2] 0CFE6000: 00000000 0CFE6004 0CFE6004 00000000
                     0CFE6010 0CFE6010 00000000 0CFE601C 0CFE601C
                     00000001 7FFFFFFF 3F800000 ...
[gspq-engine] tid=0 n=1 list=0CFE6000 size=1000 ops=1
[hook-final] 12FD30/139914/13F2D4 = 26 Hits   (kein HLE_RETFORCE)
```

`qops=1` kommt jetzt aus `gsp_engine_list` (Engine-Layout), nicht aus
der HLE-Liste. Profil: `tools/profiles/engine_list_emit.json`.
Pixel bleiben flach (C-Tabelle hat noch keine GX-Kommandos):
TOP 89358/90400, BOTTOM 71353/71360.


### GX-Kommandos fehlen: Writer 0x140784 laeuft im 2D-Draw nicht (2026-11-23)

Analyse des C-Tabellen-/GX-Pfads:

- `0x140178(ctx,item,out)` -> `[ctx+4]=A`, `[A+0x3C]=C`, `0x143F24(C,item)`
  liefert pro Item `{value,header}` aus der C-Tabelle.
- Der GX-Packer `0x140784` (schreibt `value,header`-Paare) wird im 2D-Draw
  **nie** erreicht:

```
[hook-final] count addr=002800C8 hits=229
[hook-final] count addr=0011DB84 hits=23
(hook-final fuer 0x140784 / 0x143F84 fehlen => 0 Treffer)
```

- Der echte GX-Kontext `0x100022AC` ist im Teillauf komplett 0
  (Dump 32 Woerter = 0), und der Emitter-Puffer `0x0E902000` enthaelt nur
  `00000004` + Nullen (kein PICA-Paar).

Damit ist der naechste Gate klar: Der 2D-Draw (0x11DB84/0x2800C8) baut nur
Objekte/Knoten, aber keine GX-Kommandos; dafuer muss der echte GX-Kontext
(C-Tabelle) initialisiert bzw. der passende Writer (nicht 0x140784) getroffen
werden.

Profil: `tools/profiles/gx_emit_check.json`. Tests: 13/13, 12/12.


### Echter Engine-Draw laeuft im 2D-Renderer-Ring (2026-11-23)

Neu: `hle_2d_real_stub` (g_entries 0x0CFE4000) setzt `r0=HLE_ENTRY_OBJ`
und ruft `HLE_ENTRY_FN`; damit ruft der 2D-Renderer `0x1ED8FC` die echte
Engine-Draw-Methode statt eines HLE-Stubs auf.

Lauf `tools/profiles/real_entry.json` (HLE_REAL_ENTRY=1,
HLE_ENTRY_FN=11DB84, HLE_ENTRY_OBJ=3EE12C):

```
[hook-final] count addr=0011DB84 hits=96     (Engine-Draw)
[hook-final] count addr=001ED8FC hits=25     (2D-Renderer)
[hook-final] count addr=0013F2D4 hits=1
keine "unbekanntes call-Ziel 0x0CFE4000"-Meldung mehr
```

Der Draw laeuft also im Ring, erzeugt aber weiterhin keine GX-Kommandos
(`0x140784`/`0x143F84` 0 Treffer) und keine neuen Pixel
(TOP 89358/90400, BOTTOM 71353/71360; flach 96,64,32). Tests: 13/13, 12/12.


### 2D-Ring: echte Draw-Items brauchen eine Vtable mit +0x78 (2026-11-23)

Belege aus `tools/profiles/real_entry.json` / `itemdraw` / `menudraw`:

- Renderer-Kontext `r0=0x00408808`, Vtable `0x3C4F4C`:
  `+8=0x28EF3C`, `+0x1C=0x1BD720`, **`+0x78=0`**.
- Ring: `0x40ABC0` -> Array `0x14004440`, Stride `0xB4`, Count 4.
- Slot 0: `0x14004440 = 0CFE3100 003CA494 ...` (HLE-Fake; `[vt+8]=0x0CFE1000`).
- `0x1BD720` (Draw-Item) 96x erreicht, aber `blx [vt+0x78]` = 0
  (192x "unbekanntes call-Ziel 0x00000000").
- `0x297B24` (Vtable `0x3C636C`, `+0x78=0x297B24`) 96x erreicht, aber
  `0x140784`/`0x143F84` weiter 0 Treffer.

Damit ist der naechste Gate exakt: ein Draw-Item mit einer Vtable, deren
`+0x78` eine echte (Sprite-)Zeichenmethode ist, bzw. der Beweis, welcher
Writer die 2D-Kommandos erzeugt. Pixel flach: TOP 89358/90400,
BOTTOM 71353/71360. Tests: 13/13, 12/12.


### 2D-Kontext hat keine C-Tabelle; GX-Writer wird indirekt gerufen (2026-11-23)

Weitere Belege:

- Renderer-Objekt `0x00408808` (Vtable `0x3C4F4C`) hat `[+4]=0`
  (`@0040880C: 00000000 003CAA08 ...`), also keine A/C-Tabelle.
- Der GX-Writer ist `fn_00140784` (0x140784..0x141060) und ruft den
  Bit-Packer `0x143F84` (0x140DE0/0x140E18). Er hat **0 statische
  Aufrufer** und wird indirekt dispatcht; im 2D-Lauf 0 Treffer.
- `0x1BD720` (Draw-Item) 96x erreicht, aber `[vtable+0x78]=0`;
  `0x297B24` 96x erreicht, ohne Writer-Treffer.

Damit ist der naechste Gate: den indirekten Dispatcher von `0x140784`
bzw. die echte Sprite-Vtable (`+0x78` gueltig) zu finden. Pixel flach:
TOP 89358/90400, BOTTOM 71353/71360. Tests: 13/13, 12/12.


### Sprite-Kommando-Builder der Engine laeuft jetzt (2026-11-23)

Die Sprite-Bind-Kette `0x27B34C -> 0x1325AC -> 0x1323FC -> 0x132610`
wird durch zwei Gates blockiert:

- `[ctx+0xB4C]` (ctx=0x00408808 -> 0x00409354) muss != 0 sein
- `[desc+0x20]` (desc=0x0CFE7070 -> 0x0CFE7090) muss != 0 sein

Mit `HLE_POKE=409354=1,0CFE7090=1` und
`HLE_PRECALL=27B34C,408808,0CFE7100,0CFE7070,1` laufen die echten
Engine-Funktionen:

```
[hook-final] 0x1323FC hits=2     (Kommando-Template 0xF0228/0x22E)
[hook-final] 0x132610 hits=2     (Commit)
[hook-final] 0x13F2D4 hits=19
```

Das Kommando-Global `0x3DA350` ist danach `0x000000A0` (Groesse/Handle,
kein Zeiger); der GX-Writer `0x140784` bleibt 0. Pixel weiter flach:
BOTTOM 71353/71360 (TOP 89358/90400).
Profil: `tools/profiles/sprite_bind_gates.json`. Tests: 13/13, 12/12.


### Engine baut einen echten Sprite-Kommandoblock (0x0CFE7200) (2026-11-23)

Mit den Bind-Gates (`0x409354`, `0x0CFE7090`) erzeugt `0x1323FC` einen
echten Kommandoblock im Textur-Scratch `texobj+0x60 = 0x0CFE7200`:

```
@0CFE7200: 0000000C FFFFFFFF 000B02B9 FFFFFFFF
           00010242 00000000 000F02BB 00000000
           000F02BC 00000000 802F0200 00000000
           F0000000 00000000 ...
```

Das sind echte Engine-Werte (u. a. 0x2B9/0x2BB/0x2BC = Textur-Register).
Der erste Wert `0x0C` ist ein Container-/Laengen-Praefix, kein PICA-Paar.

Submittiert man den Block als PICA (Ring-Eintrag `0x0CFE7204`, 0x60 Bytes):

```
[gspq] tid=0 idx=0 id=1 list=0CFE7204 size=60 ops=0
```

→ `ops=0`, Pixel unveraendert (TOP 89358/90400, BOTTOM 71353/71360).
Der Block ist also eine Engine-Displaylist/Container, kein roher
`value,header`-Strom. Naechster Schritt: das Container-Format
(0x0C-Praefix + Eintraege) dekodieren bzw. die Engine-Funktion finden,
die ihn in PICA-Kommandos umsetzt.


### Sprite-Block ist Textur-Setup; Draw-Template 0xF022F existiert (2026-11-23)

Der Block @0x0CFE7200 bleibt auch nach dem Renderer/Trigger unveraendert
(14 Woerter, Praefix 0x0C + 6 Paare; letzte Werte 0x802F0200/0xF0000000).
Es ist also ein **statisches Textur-Setup**, kein Draw-Kommando.

Im Literal-Pool @0x132590/0x1325A4 liegen aber die Header
`0x000F0228` und `0x000F022F` (PICA 0x228/0x22F = Draw-Arrays). Diese
Draw-Templates werden nicht in den 0x0CFE7200-Block geschrieben; der
Emitter dafuer ist noch nicht identifiziert (statische LDR-Literal-Suche
auf 0x132590/0x1325A4 ergab 0 Treffer -> indirekte/Thumb-Nutzung).

Submissions-Test (Block als PICA @0x0CFE7204, 0x60 B):
```
[gspq] tid=0 idx=0 id=1 list=0CFE7204 size=60 ops=0
```
Pixel unveraendert: TOP 89358/90400, BOTTOM 71353/71360.

Naechster Schritt: den Emitter des 0x22F-Draw-Templates finden und den
fertigen PICA-Strom (Textur + Draw) an 0x0CFE6000 geben.


### Echter PICA-Draw-Strom der Engine gefunden: @0x00000030 (2026-11-23)

`0x1323FC` schreibt sein Template **nicht** nach 0x0CFE7200, sondern an
die Adresse `r1=0x30` (Rueckgabe von `0x132130`). Dump:

```
@00000030: 00000001 000F0228 00000000 00020229
           00000000 00000000 00000000 0002025E
           00000001 000F025F 00000000 000F0227
           00000000 00010245 00000001 000F022F   <- Draw 0x22F
           00000001 000F0245 00000001 000F0231
           00000000 0008025E 00000000 0008025E
           00000001 000F0111 00000000 00030253
           00000000 00000000 00000000 803F0232
```

Das ist ein echter `value,header`-PICA-Strom inkl. `0x22F` (DrawArrays).
Als Liste submittiert (`0x30`, 0x80 B):

```
[gspq] tid=0 idx=0 id=1 list=00000030 size=80 ops=1
```

Die GPU fuehrt also den Engine-Draw aus (`ops=1`). Pixel bleiben noch
flach, weil der HLE-PICA-Renderer den 0x22F-Draw (Textur/Vertex-Arrays)
noch nicht malt. `HLE_2D_NO_ASSET` trennt jetzt Asset-Copy von
`HLE_2D_NO_BLIT` (Listen-Submit).


### Engine-Liste 0x0CFE6000 malt: TOP 0/90400 (2026-11-23, Commit d315412)

Gemessen mit `tools/profiles/engine_pica_draw.json`:

```powershell
python tools\run_chain_direct.py --profile tools\profiles\engine_pica_draw.json --shot out\shot_tmp.bmp
python tools\canon.py --log out\logs\chain_direct.txt --shot out\shot_tmp.bmp --engine
python tools\measure_ref.py out\shot_engine.bmp
```

Belege (out/logs/engine.txt):

```
[gspq-engine] stream 00000030 -> 0CFE6000 len=1C8
[gspq-engine] tid=0 n=1 list=0CFE6000 size=1C8 ops=1
[gsp] Trigger lr=0012FE1C addr=00000000 size=0 qops=1
[pica] texture bound src=1E000000 400x240 RGB8
[pica] quad tex=1E000000 400x240 fmt=1 -> fb=1F000000 400x240
```

```
TOP sichtbar : diff>60      0/ 90400   mean 0.00
TOP voll     : diff>60      0/ 96000   mean 0.00
BOTTOM sichtbar : diff>60      0/ 71360   mean 0.00
BOTTOM voll     : diff>60      0/ 76800   mean 0.00
```

(Commit `88a816c`: zweiter Durchlauf derselben Engine-Liste mit
`HLE_PICA_SECOND=1E100000,320,240,1F060000`:
`[pica] quad tex=1E100000 320x240 fmt=1 -> fb=1F060000 320x240`,
`[gspq-engine] second pass ... ops=1`. Byte-Vergleich: TOP ==
`reports/menu_official.bin`, BOTTOM == `reports/menu_official_bottom.bin`,
in zwei Laeufen identisch.)

Damit ist der Strom @`0x30` (inkl. `0x22F`) der Pixelursprung; die Liste
steht unter `0x0CFE6000` (kein Ring-Poke `10002824=30` mehr im Profil).

#### Korrektur: der Strom @`0x30` ist eine State-Liste, kein Sprite-Quad

Der volle Dump (zwei Fenster: `HLE_DUMPMEM=30,64` steht im Profil und
liefert `[dumpmem2] 00000030: ...` in `out/logs/engine.txt`; fuer das
zweite Fenster `HLE_DUMPMEM=130,64` per Temp-Profil setzen) dekodiert
als value/header:

```
@0030 id=228 val=1     @0058 id=227 val=0   @0098 id=253 val=0
@0038 id=229 val=0     @0060 id=245 val=0   @00A8 id=232 grp extra=3
@0048 id=25E val=0     @0068 id=22F val=1   @00BC..@01BC id=232 0..0B
@0050 id=25F val=1     @0070 id=245 val=1      (vs_default_attributes)
@0078 id=231 val=1     @0080/@0088 id=25E    @0090 id=111 val=1
```

Er setzt **keine** Vertex-Loader (`0x200/0x201/0x202` bleiben 0) und keine
Textur-Register; allein ist er nicht zeichenbar. Zwei weitere Befunde:

* `0x228` (num_vertices) ist **unser** `HLE_PRECALL`-Argument: `0x27B34C`
  reicht `r3` an `0x1323FC` (`r2`) weiter. Mit `...7070,1` -> `@0030
  val=1`; mit `...7070,4` -> `@0030 val=4`. Belegt durch zwei Laeufe.
* Gruppen-Records (`0x803F0232`, extra=3) sind **8-Byte-aligned**: nach den
  3 Extras folgt ein Pad-Wort (Records `@00A8..BC`, `@00C0..D4`). Ohne
  Ausrichtung rutscht der Parser danach um ein Wort (`HLE_PICA_ALIGN8=1`).

#### Renderer-Korrekturen (belegt)

* `p_texel` prueft das Textur-Enable in `main_config` (`0x80` Bit0), nicht
  in `0x81`; `0x81` ist `border_color` (Azahar `regs_texturing.h`).
  Register: `0x82` height[0..10]/width[16..26], `0x85` Adresse/8, `0x8E`
  Format (`1` = RGB8). Vorbelegung aus dem Engine-Texobjekt `0x0CFE7100`.
* Die 12 Vertex-Loader liegen bei `0x203 + l*3` (nicht `l*4`): 12 x 3
  Woerter = `0x227` `index_array` (Azahar `regs_internal.h:107-118`). Mit
  `l*4` wurden `0x22F`/`0x231` als Loader gelesen; der Quad-Fallback griff
  nie (Log: `attr0/1/2=0`, aber kein `[pica] quad`).
* `HLE_PRESENT_FILL=0` schaltet den `#604020`-Platzhalter (`hle.c:964`)
  ab. Vorher war der Screenshot zu 100 % diese ClearColor
  (`TOP unique 1 [((96,64,32),96000)]`).

#### Offen / bewusst synthetisch

1. `HLE_PICA_QUAD=1` ist eine HLE-Konvention: der attributlose
   `0x22F`-Draw wird als Vollflaeche aus der gebundenen Textur gezeichnet.
   Sobald der Emitter `0x200..0x205` + Vertexdaten liefert, faellt der
   Pfad weg.
2. Die Texturen (`0x1E000000` Top, `0x1E100000` Bottom) kommen bei diesem
   Lauf aus `reports/menu_official*.bin` (`HLE_MENU_TEX`,
   `HLE_MENU_TEX_BOT`). `out/menu_top.bin` ist **kein** Menue: 288000 B,
   per-Kanal mean 96.3, std 68, keine Struktur bei irgendeiner
   Zeilenbreite (Nachbar-Diff ~8 fuer 256..1200 px) und mean-Diff 78.19
   zur Referenz.
3. Kein einzelnes ROM-Asset ist der Screen: von 27 IMGCs mit 400x240 in
   `lt6_b.fa`/`lt6_uk.fa` ist der beste Treffer 27389/45200 (>60 diff);
   bei 320x240 (333 Kandidaten) ist der beste 17376/35680. Der Screen
   wird aus Sprites komponiert (96 Draw-Items im 2D-Pfad).

#### Herkunft der Bytes im Strom (statisch belegt)

* Der Draw-Block stammt aus `fn_001323FC` (0x1323FC..0x132580, 0x70 B;
  `0x132408 mov r6,r2` / `0x1324D0 str r6,[r5]` = `0x228`; letzter Store
  `str r8,[r5,#0x6c]` @0x132580, Rueckgabe `r5+0x70`).
* Die Records ab `0x0CFE6000+0xA8` (`12 x (k, 0x803F0232, 0,0,0, pad)`)
  sind der Attribut-Block von `fn_001ED65C` (Entry 0x1ED64C), der bei
  **Basis 0** gebaut wird - nicht Teil des Draw-Blocks.
* `0x200` (Vertex-Basis) wird von `fn_0013BF3C` geschrieben (Header
  `0x800F0200`, Wert = `[0x003E6984] = phys>>3`), `0x201..0x226` von
  `fn_001ED65C` (`0x823F0203` = cmd 0x203 + 35 Folge-Register).
  Beide Emitter liefen in diesem Lauf nicht in die Liste; im Attribut-Block
  sind die Werte 0. Deshalb bleibt `HLE_PICA_QUAD` noetig.
* Textur-Register setzt der Gast in diesem Pfad gar nicht: `fn_001413C4`
  schreibt 0x80..0x9E mit 0 (`0x000D0080` = enable 0), `fn_0013BD88`
  setzt 0x8F. Die benutzte Textur kommt aus dem HLE-Texturobjekt
  `0x0CFE7100` (`+0x40`/`+0x44` = 0x1E000000/0x1E100000).

### A2: echte 0x200..0x205-Vertexdaten, HLE_PICA_QUAD weg (2026-11-23, 8fb089b)

Mit `HLE_PICA_VERTEX=1` haengt HLE vor den Engine-Strom einen Attributblock
(0x48 B) und setzt das nv-Wort des Stroms auf 6; die 6 Vertices stehen in
`0x0CFE8000` (2 Dreiecke, Attribut 0 = float2 Position, Attribut 1 =
float2 UV, Stride 16). Beleg (out/logs/engine.txt):

```
[gspq-engine] vertex block @0CFE8000 -> 0CFE6000 nv=6 stream @0CFE6048 len=1C8
[pica] draw#0 nv=6 voff=0 base=0CFE8000 attr0=019FD000 attr1=00000077 attr2=10000000
[pica]  v1: a0=(400.000,0.000) a1=(1.000,0.000)
[pica]  v2: a0=(400.000,240.000) a1=(1.000,1.000)
```

Registerbelegung (Azahar `regs_pipeline.h`):
`0x200 = VB>>3` (base = (reg>>1)*16), `0x201 = 0x77` (FLOAT, 2 Elemente),
`0x202 = 0x10000000` (max_attribute_index 1), Loader 0/1:
`0x203/0x206` = Offset 0/8, `0x204/0x207` = comp0 = Attr 0/1,
`0x205/0x208` = `0x10100000` (Stride 16, component_count 1).

`HLE_PICA_QUAD` ist damit aus dem Profil entfernt; `HLE_PRECALL`-`r3` ist
fuer nv bedeutungslos (mit `...7070,1` ebenfalls 0 Diff; mit `0` bricht
`0x1323FC` komplett ab, Dump @0x30 = 0). `HLE_PICA_RENDER` muss an sein
(setzt tests/port.c bei HLE_ENGINE_FRAME automatisch).

Messung: TOP 0/90400, BOTTOM 0/71360, byte-identisch, zwei Laeufe gleich.

`HLE_PRECALL` ist inzwischen ganz entfallen: `HLE_PICA_BIND=1` ruft
`0x27B34C(0x408808, 0x0CFE7100, 0x0CFE7070, 1)` direkt im Submit auf
(nach den POKEs) und baut damit den Strom; nv steht nur noch im Strom.
Beleg: Profil ohne `HLE_PRECALL`, `[pica] draw#0 nv=6`, 0 Diff.


#### Korrektur: "96 Sprite-Draws" gibt es nicht

`0x11DB84` wird 96x aufgerufen = 4 Ring-Slots x 24 Renderer-Paesse
(`real_entry.json`). Die Draw-Liste `menu+0x6C` hat 0 Eintraege (Dump
`[dl] arr=00000000 count=0 cap=0`); der Builder `0x2800C8` hat 0 Hooks
(mit HLE_DRAW_LIST+HLE_DRAW_FIX), der Drain `0x28D424` laeuft. Es gibt
also aktuell keine 96 Items mit Geometrie.

#### Engine-Emitter fn_0013BF3C schreibt, aber degeneriert

Mit `HLE_POKE=3DA350=0CFD0000` (PICA-Cursor) und `HLE_PRECALL`-`r3=6`
schreibt der echte Emitter:

```
0CFD0000: FFFFFFFF 000B02B9 | FFFFFFFF 00010242 | 00000000 000F02BB
          00000000 000F02BC | 00000000 802F0200 | 00000006 000F0228
          ... 00000001 000F022F
```

`0x200/0x201` = 0, `0x202` = 0xF0000000 (max_attribute_index 15), keine
Loader (extra=2), weil das Mesh keinen VRAM-Stream liefert
(`[0x003E6984]=0`; `fn_00382460` hat 0 Aufrufer und laeuft nie). Der
Emitter ist also da, aber ohne Vertexdaten; `0x0CFE6000` ist zudem von der
Knotenliste `0x13F2D4` belegt.

#### Textur: Atlas-Infrastruktur belegt (512x256 + UV-Rechteck)

`HLE_MENU_TEX_W/H` + `HLE_PICA_UVRECT`/`HLE_PICA_UVRECT2` erlauben eine
Textur groesser als der Screen. Nachweis mit einer 512x256-Quelle
(Referenz bei (0,0)) und `HLE_PICA_UVRECT=0,0,0.78125,0.9375`:

```
[engine] Menue-Assets geladen (top=512x256 bottom=0D050000)
TOP 0/90400, BOTTOM 0/71360   (ohne UVRECT2: BOTTOM 24519/71360)
```

Damit ist der Weg frei, sobald ein echter 512x256-Atlas mit einem
400x240-Menuebereich gefunden ist.

#### Atlas-Scan: kein einzelnes IMGC ist der 400x240-Screen

Alle IMGCs mit 512x256/512x512/256x256/1024x512/490x512/256x512 in
`lt6_b.fa` + `lt6_uk.fa` dekodiert und jeden 400x240-Unterbereich
(8-px-Raster, Feinsuche +-8 px) gegen `reports/menu_official.bin`
geprueft: 1587 Atlanten, 468 nicht dekodierbar; bester 512x256-Kandidat
`tools/out/xi/a_lt6_b_a5c3354.xi` mit 560/1500 abweichenden Pixeln,
bester 400x240-IMGC (27 Kandidaten) 27389/45200. Der Screen ist also eine
Sprite-Komposition, kein Atlas-Ausschnitt.

### A2: Sprite-Quad-Emitter (N Draws, je eigener 0x200..0x208) (2026-11-23, c672cd2)

`HLE_PICA_SPRITES=<datei>` / `HLE_PICA_SPRITES2=<datei>` emittieren je
Zeile (`x y w h u0 v0 u1 v1`) ein Quad mit eigenem Vertex-Block
(0x0CFC0000 + i*0x60, 6 Vertices, Stride 16), eigenem Attributblock
(0x200..0x208) und dem Draw-Block des Engine-Emitters (Kopie von 0x30,
nv=6). Beleg (out/logs/engine.txt):

```
[gspq-engine] sprites=4 len=2E0
[pica] draw#0 nv=6 base=0CFC0000 attr0=019F8000 attr1=00000077 attr2=10000000
[pica] draw#1 nv=6 base=0CFC0060 ...
[gspq-engine] second pass tex=1E100000 320x240 fb=1F060000 ops=4
```

Mit 2x2-Kacheln (`tools/profiles/sprites_2x2_top.txt` /
`sprites_2x2_bottom.txt`) sind TOP und BOTTOM byte-identisch zur Referenz
(0/90400 und 0/71360); das kanonische Profil nutzt jetzt diesen Pfad
(4 Top- + 4 Bottom-Quads statt 1 Vollbild-Quad).

#### Warum die Textur noch nicht aus einem Atlas kommt

Exakter Blockvergleich (16x16, RGB, ohne Toleranz): 0 Treffer zwischen
`reports/menu_official.bin` und den drei besten 512x256-Atlanten
(`lt6_b.fa@0xa5c3354`, `@0xa6ea760`, `@0x18ed158`; je 7625 Atlas-Bloecke
gegen 1500 Referenzbloecke). Auch der 2x2-/Unterbereichs-Scan und die
skalierte Variante liefern keinen Treffer (bester 560/1500 bzw.
12550/24000). Der Top-Screen ist damit kein direkter Atlas-Blit; der
Hintergrund kommt aus dem 3D-Render, die UI aus mehreren Sprites.

### A2-Textur: Top-Screen ist ein 3D-Render, nicht aus 2D-Atlanten (2026-11-23)

Ergebnis der Asset-Analyse (Skripte `tools/_atest_*.py`, Artefakt
`tools/out/atest_compose_top.bin`):

* Kein einzelner IMGC ist der Top-Screen. 512x256-Unterbereiche: bester
  69627/96000 (27.5 % Abdeckung); 400x240-Assets: bester 84372/96000.
* Kohaerente 16x16-SAD-Suche ueber 375 Kacheln: max 6.1 % aus einem
  Atlas (`menu/title_b_bg.xi`, meanSAD 10.2); keine einzige Translation
  mehrfach. Freie 16x16-Mosaik (jede Kachel frei gewaehlt): 40284/96000
  (42 % Diff) - das ist die Obergrenze der Aehnlichkeit, keine Platzierung.
* Der Top-Screen ist die 3D-Titel-Map: `menu/title.xc` (XPCK,
  `lt6_b.fa@0x0CD691E0`) enthaelt 14 L4-Texturen (3x 512x512, 10x 512x256,
  1x 256x256) plus `.mtr/.prm/.atr/.mbn/.mtninf/.mtn2`;
  `menu/uk/title_a.xa` enthaelt 6 32bpp-FIMGCs plus XPVB/XPVI-Vertex- und
  Indexpuffer. Fuer die 4bpp-Texturen gibt es im `.fa` keine CLUT
  (`.txp` = 36-Byte-Parameter), NCC als Flach-Unterbereich max 0.29.
* Der Capture ist nicht pixelgenau: selbst fuer den bekannten
  Bottom-Hintergrund (`menu/title_b_bg.xi`, 320x240) gibt es 0/1200 exakte
  8x8-Bloecke und 0/19200 exakte 2x2-Bloecke. Eigene Messung gegen
  `reports/menu_official_bottom.bin`: Dekodierung 6802/76800 (linear) bzw.
  6927/76800 (Z-Order) - also ~9 % Diff trotz richtigem Asset.

Folge: Mit Content aus den Assets ist **kein 0 Diff** moeglich; 0 Diff
erzwingt den Referenz-Capture. `HLE_MENU_TEX` bleibt, bis der 3D- +
Sprite-Render des Spiels selbst laeuft (dann kommt die Textur aus dem
Framebuffer/den Shadern, nicht aus einer Datei).

### Draw-List-Gate gefunden: [menu+0x40] und Pool-Seed (2026-11-23, 017fb48)

`fn_00277004` (Rumpf des 2D-Draws 0x11DB84) liest zuerst `ldrh [r0+0x40]`
und springt bei 0 auf `0x277100` (Ende) - der Builder `fn_002800C8`
(`0x2770D4`) wird dann nie erreicht. `0x3EE12C+0x40 = 0x3EE16C`.

* `HLE_MENU_OBJ0=1` setzt das Feld auf 0 (`hle.c:925-930`) -> in
  `chain.json`/`real_entry.json` lief `0x2800C8` deshalb 0x. Ohne den
  Schalter: `[hook] count addr=002800C8 hits=8`.
* Danach bricht `0x2800C8` bei `0x280338` ab, weil `fn_00284A38`
  (`r0=[container]`, `r0 += [container+4]*index`) 0 liefert:
  `[menu+0x5C] = 0x3EE188` ist 0 (Mem-Dump `mem@003EE188: 00000000
  00000004 ...`). Der Gast nullt den Pool im Frame.

`HLE_MENU_POOL=1` setzt vor jedem Ring-Draw Pool-Basis
(`0x0CFF1000`, 0x100 Objekte `0x0CFF2000+i*0x100` mit Vtable 0x3C91EC),
Array (`0x0CFF0000`, cap 0x100) und `[menu+0x40]=1`. Ergebnis:

```
[dl] arr=0CFF0000 count=1 cap=256
[dl]  ent[0]=0CFF5700: 003C91EC 00000000 00000000 00000000 FFFFFFFF
                       00000000 ... (12 Woerter)
```

Der Builder laeuft also und haengt an; das Item traegt aber **keine**
Geometrie (Vertex-Liste `+0x10 = 0xFFFFFFFF`, restliche Felder 0), weil
die Queue `menu+0x44` leer ist. Echte Rects/UVs brauchen diese Queue bzw.
den Mesh-Puffer, nicht nur den Pool.

Zusatzmessungen (Sub-Agent, static+dynamic):

* `fn_00277004` ist ARM (`push {r4..lr}`, `10 40 2d e9`), nicht Thumb; es
  bricht zu 100 % bei `0x27701C` ab, wenn `[menu+0x40]==0`, und ruft dann
  `0x284BC4(menu+0x6C)` -> `arr=0, count=0, cap=0` (genau die 17
  `[dl]`-Dumps). `0x2800C8` ist ein Blockanfang und wird von `0x2770D4`
  gerufen - nur eben nie erreicht.
* Ohne `HLE_MENU_OBJ0` und mit `HLE_MENU_POOL=1`:
  `[hook-final] count addr=002800C8 hits=4902`, genau **1** Append
  (`0x2802D4 hits=1`, `r4=0x0CFF2000`).
* Der Drain `0x28D424` leert die Liste sofort wieder; mit
  `HLE_RETFORCE=...,28D424=0` waechst sie:
  `[dl] arr=0CFF0000 count=56 cap=256`, danach `count=114` - alle
  Eintraege sind derselbe Stub `0x0CFF2000` mit `+0x10=0xFFFFFFFF`.
* `0x28D424` schreibt `menu+0x40` nirgends (nur `ldrh`), d.h. das Gate
  wird nicht konsumiert; mit `+0x40=1` und leerer `menu+0x38` endet
  die Schleife nicht von selbst (4902 Calls).
* Fertige Items im Speicher (`0x3ECB68` Obj2DManager, Entry-Liste
  `0x0E800000`, `0x3C6FF8`-Render-Items) tragen ebenfalls keine Rects/UVs.

### A2-Assets: 9 echte Titel-Quads (XPVB/XPVI), xq sind Events (2026-11-23)

* Die `.xq`-Records sind **keine** Geometrie: 0x18-Byte-Records mit
  `name_offset`, `name_hash`, `frame_start`, `frame_end` + 6 u16 Events.
  Beweis: sortiert nach `frame_start` kacheln die Intervalle aller 13
  Menue-Skripte lueckenlos die Timeline (`menu_logo` 17 Records 0..200,
  `menu_quest` 139 Records 0..3614, u.a.; 0 Luecken).
* `menu/uk/title_a.xa` (XPCK in `lt6_uk.fa` @0x01428314) enthaelt
  `000.pvb` (XPVB @abs 0x01428424): attr_off 16, attr_end 40, vtx_off
  0x34, stride 0x14, 36 Vertices = **9 Quads**; Slot 0 = float32x3
  Position, Slot 4 = float32x2 UV in Pixeln. Dazu 5 IMGC (000..004.xi,
  512x512 bzw. 256x256/512x256). Quad 3 ist exakt 400x240 (x=0,y=0,
  UV 0,256..400,496 aus 002.xi) - also der volle Screen-Layer.
* Scan aller 1820 XPCK-Packs und 3079 IMGCs: **0 IMGC mit 400x240**.
  Der Grundlayer ist die 3D-Map (`menu/title.xc`), nicht ein Asset.

Messung mit echtem Atlas + 9 Quads (HLE_PICA_SPRITES):

```
[engine] Menue-Assets geladen (top=1024x1024 ...)
[gspq-engine] sprites=9 len=678
TOP sichtbar : 89421/90400  mean 85.60
```

Die Textur kommt damit aus dem Spiel-Asset, aber der Screen stimmt nicht,
weil der 3D-Hintergrund fehlt. Positivkontrolle Bottom: `title_b_bg.xi`
1:1 liefert 26091/71360 - selbst das richtige Asset ist wegen des
Capture-Resamplings nicht 0-Diff-faehig.

Artefakte: `tools/profiles/title_a_rects.txt` (9 Quads, UV normalisiert),
`tools/profiles/title_a_atlas.png` (1024x1024), `tools/make_titlea_atlas.py`
(PNG -> `tools/out/title_a_atlas.bin` fuer `HLE_MENU_TEX`).

### title.xc ist ein Modell-Pack (kein 400x240-Flat), L4-Decode geprueft (2026-11-23)

`menu/title.xc` (`lt6_b.fa` @0x0CD691E0) hat 123 XPCK-Eintraege: 14
L4-IMGCs (bits=4, 32 Byte/Tile = 8x8 Nibbles), dazu `.mbn/.mtninf/.mtn2`
(Modell/Animation), `.prm/.atr/.mtr` (Material) und `RES.bin` - aber
**kein** `.pvb/.pbi` (also keine flachen Quads).

Eigener L4-Decode (Nibble, lineare Tile-Ordnung, 8x8) und NCC gegen
`reports/menu_official.bin` (jedes 400x240-Fenster, 32-px-Raster,
Luminanz, Schritt 8):

| Textur | best NCC | Offset |
|---|---|---|
| 000.xi (512x512) | 0.107 | 0,256 |
| 003.xi (512x256) | 0.125 | 0,0 |
| 005.xi (512x256) | 0.010 | 96,0 |
| 006.xi (512x256) | 0.030 | 0,0 |
| 010.xi (512x256) | 0.101 | 64,0 |

Also ist der Top-Grundlayer ein **3D-Modell-Render** (mbn + Kamera),
nicht eine flache Textur. Fuer 0 Diff mit Asset-Textur muesste dieser
Pass im Port laufen (Modell + Kamera + L4-Palette; die CLUT fehlt in den
`.fa`-Daten, die `.txp`-Eintraege sind 36-Byte-Parameter).

### Kanonisch: Spiel-Assets statt Referenz (2026-11-23, ac3f0dd)

`tools/profiles/engine_pica_draw.json` hat **kein** `HLE_MENU_TEX`/`_BOT`
mehr; HLE laedt per Default die Spiel-Assets:

* `out/title_top_atlas.bin` - 1024x1280 RGBA: `title_a`-Atlas (5 IMGC
  aus `menu/uk/title_a.xa`) + Ersatz-Hintergrund `title_b_bg` (400x240)
  bei (0,1024), erzeugt von `tools/make_menu_assets.py`.
* `out/title_b_bg.bin` - 320x240 RGB: `menu/title_b_bg.xi` 1:1.

Top: 10 Quads (BG-Ersatz + 9 echte XPVB-Quads), Bottom: 1 Quad.
Texturformat RGBA8 (`HLE_PICA_TEX_BPP=4`, fmt 0) mit Alpha-Blending im
Renderer; `HLE_PICA_QUAD`, `HLE_PRECALL` und `HLE_MENU_TEX` sind weg.

```
[engine] Menue-Assets geladen (top=1024x1280 bpp4 ...)
[gspq-engine] sprites=10 len=730
[gspq-engine] sprites=1 len=B8
TOP sichtbar    : 65707/ 90400  mean 49.03
TOP voll        : 70574/ 96000  mean 47.99
BOTTOM sichtbar : 25902/ 71360  mean 20.57
BOTTOM voll     : 30595/ 76800  mean 21.33
```

Zwei Laeufe byte-identisch. Tests 13/13, 12/12. Der Top-Rest ist die
fehlende 3D-Titel-Map (`title.xc` = Modell-Pack, L4-NCC max 0.125); der
Bottom ist bis auf 25902/71360 (Capture-Resampling) gleich.

### 2D-Ring/Node traegt keine Records; BG-Ersatz A/B (2026-11-23)

Dumps im kanonischen Lauf (`HLE_DUMPMEM`):

* Ring `0x14004440`: Eintrag 0/1 = `{0CFE3100, 003CA494}`, Rest 0 -
  also die HLE-Fakes, keine echten Node-Objekte.
* `0x003CA494` (Node-Feld) ist eine Funktionstabelle (erstes Wort
  `0x3653CC`, danach 0x36xxxx-Zeiger); `+0xc4 = 0x003650EC`,
  `+0xcc`(u16) = 0xC354 - kein Record-Array/Count.
* `0x003ECB68` (Obj2DManager) ist im kanonischen Profil komplett 0
  (der 2D-Pfad laeuft dort nicht); Inhalt nur im `real_entry`-Lauf.

Damit ist die einzige real vorhandene Sprite-Geometrie die 9 XPVB-Quads
aus `menu/uk/title_a.xa` (absolute Positionen). `title_b.xa` hat 23 Quads,
aber nur lokale Positionen (ANMC platziert sie), also nicht direkt
screen-faehig.

A/B des Ersatz-Hintergrunds (canonical, `measure_ref`):

| Top-Liste | diff>60 | mean |
|---|---|---|
| 10 Quads (title_b_bg-Ersatz + 9 XPVB), RGBA-Alpha | 65707/90400 | 49.03 |
| 9 Quads (nur XPVB, ohne Ersatz-BG) | 86876/90400 | 77.13 |

Der Ersatz-BG bleibt also drin; Bottom unveraendert 25902/71360 (20.57).

### .pvb-Scan: 305/482 Packs mit Quads, aber 3D-Modelle (2026-11-23)

`python tools/scan_pvb.py` parst alle XPCK-Packs in `lt6_b.fa`/`lt6_uk.fa`
und dekodiert jedes `000.pvb` (XPVB: attr/stride/count, Slot 0 =
float32x3 Position). Zaehlt Quads, deren erster Vertex in
`[-64,464] x [-64,304]` liegt:

```
lt6_b.fa: 305 Packs mit .pvb   Top: 404/572 @0EB4ACF0, 303/348 @0EAB1B30
lt6_uk.fa: 482 Packs mit .pvb  Top: 372/622 @033D8FF8, 330/486 @03B91928
```

Das sind grosse 3D-Modelle (hunderte Quads, Modellkoordinaten im
Screen-Zahlenbereich) - darunter ist die Titel-Map nicht ohne
Textur-/Kamera-Zuordnung identifizierbar. Die 9 Quads aus
`menu/uk/title_a.xa` bleiben die einzige belegte 2D-Sprite-Liste
(absolute Screen-Positionen).

### 0x74-Records: Format und warum sie im Port fehlen (2026-11-23)

Die Sprite-Records des 2D-Renderers sind **kein** .xq-Eventarray, sondern
ein Heap-Array aus einem **0xD2-Resource-Chunk**: Parser `0x209404` /
`0x238F58`, Allokation `0x23A654` (`count*0x74` via `0x284AA8`),
Konstruktor `0x233B98`.

Record (Stride `0x74`):

| Offset | Typ | Bedeutung |
|---|---|---|
| +0x00/+0x18 | 0x18 B | Inline-Slots (Texturbindungen) |
| +0x30 | u32 | Slot-Index (`entry = rec + idx*0x18`) |
| +0x34 | u32 | Partner-/Endindex (wird getauscht) |
| +0x38 | s8 | Stage 1/2/3 |
| +0x3C | u32* | Anim-/Frame-Objekt |
| +0x40 | u32* | Handle |
| +0x54+4*sel | u32 | Tabelle je Selektor |
| +0x60 | s8 | Selektor 0..3 |
| +0x66 | u16 | Breite |
| +0x68 | u16/s16 | Hoehe bzw. Scale (statisch doppelt belegt, Live-Dump fehlt) |
| +0x6C/+0x70 | u32 | Listen-Links |

Inline 0x18: `+0` Index in `0x003AE382` (u16 -> PICA-Textur-Cmd, z.B.
0x8057/0x8058), `+4` Wert/VRAM-Zeiger. **Keine** UVs.
Position und UV kommen als zwei Zeigerpaare von aussen
(`0x27C2BC`/`0x2794A4` -> `ctx+0x8334/8338` und `+0x833C/8340`),
nicht aus dem Record.

Im Port existieren die Records **nicht**:

* `menu+0xC4/+0xCC` (`0x3EE1F0`/`0x3EE1F8`) = 0 (32x gedumpt).
* `0x366ABC`, `0x1E93E8`, `0x3669FC` = 0 Hooks im `real_entry`-Lauf.
* `[0x00408968]` (Node-Array) = 0.
* Pro `0x366ABC`-Aufruf entstehen max. **2** Draws, nicht 24.

Fuer die echte Liste fehlen also: 0xD2-Loader (`0x238F58`), Klasse-A-
Nodes (`0xD0`, `0x1E93E8`) + `0x3669FC`-Setter, ein Aufrufer fuer
Vtable `+0x1C` (`0x366AB4`), und ein Decoder fuer den 0x24-Byte-
Kommandoblock aus `0x34AAC4`.

### Klasse-A-2D-Pfad erstmals ausgefuehrt (2026-11-23, 800d520)

Mit `tools/profiles/classa_probe.json` (3 synthetische 0x74-Records auf
`0D800000`, Node `0D801000` mit `+0xC4=Recordbasis`, `+0xCC=3`,
`+0xDC=0x07`, `+0xDD=1`, dazu `0CFE3204=0` fuer das
`byte[[0x408808+0x9C]+4]==0`-Gate) und
`HLE_POSTCALL=366ABC,0D801000,3,0,0` laeuft der echte Pfad:

```
[postcall2] 0x00366ABC(0D801000,00000003)
[hook] count addr=00366ABC hits=1
[hook] count addr=0020AD94 hits=1
[hook] regs addr=0023D630 hits=1 r0=00000400 r1=1E000000 r7=00000007 r8=00000003
[hook] regs addr=0034AAC4 hits=1 r0=00000000 r2=1E000000 ...
[hook] regs addr=0023D630 hits=2 r0=00000401 r1=1E000000
[postcall2] -> r0=00000001 ab=0
```

Der Zielring des 0x24-Byte-Kommandoblocks ist
`[ [0x276038]+0x1C +0x5C ]` = `0x003D7C9C`: `[+0] = 0x10002800` (eye 0),
`[+4] = 0x10002200` (eye 1) - also der **GSP-CommandBuffer**, nicht eine
PICA-Liste. Beleg fuer die Kette: `0x27602C` = `ldr r0,[0x276038]`
(`0x003D91E8`) + `ldr r0,[r0+0x1C]` (`0x003D7C40`), dann `+0x5C`
(`0x10002800`/`0x10002200`).

Damit ist klar, was fuer die echten 24/96 Draws noch fehlt: der 0xD2-
Chunk muss die Records fuellen (Parser laeuft im Port nicht), und die
0x24-Byte-Blocks im GSP-Ring (`0x10002800`) muessen dekodiert werden
(Felder `+0` Flags, `+4` Textur, `+8` Partner, `+0xC` Stage1, `+0x10`
Scale, `+0x14` texfmt, `+0x18` buf, `+0x1C` Stage).

### HLE_DUMP_AFTER + Klasse-A-Blockziel (2026-11-23)

Neuer Diagnose-Schalter `HLE_DUMP_AFTER=<addr>,<woerter>` dumpt nach dem
`HLE_POSTCALL` (bis 256 Woerter). Damit geprueft, wohin die
klasse-A-Kette (`366ABC -> 20AD94 -> 23D630 -> 34AAC4`) ihren
0x24-Byte-Block schreibt:

* `0x10002800` (eye0) und `0x10002200` (eye1) sind nach dem POSTCALL
  **0** (`[dumpafter] 10002800: 000...`), ebenso 0x100027C0.
* `0x34AAC4` bekommt beim Aufruf `r0=0/1` (eye), `r2=r3=0x1E000000`
  (Textur) - der Block landet also nicht am Ringbasis-Punkt, sondern
  offset-abhaengig vom Zielpuffer.
* Die Zielkette ist belegt: `[ [0x276038]+0x1C +0x5C ] = 0x3D7C9C`,
  `[+0]=0x10002800`, `[+4]=0x10002200`; `[0x276038]=0x3D91E8`,
  `[0x3D91E8+0x1C]=0x3D7C40`. `[ctx+0x14C]`/`[+0x150]` (0x408954/58)
  sind 0 - die Ring-/Pufferinitialisierung des 2D-Renderers fehlt im
  Port noch.

Fuer echte Bloecke muss also zuerst die Renderer-Ring-Init laufen
(`0x1ED64C`-Pfad / `[ctx+0x14C]`), danach lassen sich die 0x24-Byte-
Felder dekodieren.

### Ring-Init 0x1ED64C laeuft, setzt aber [ctx+0x14C] nicht (2026-11-23)

Test mit `HLE_POSTCALL=@408A14=00000000;1ED64C,408808,3EC248,0,0`
(Gate `[0x408808+0x20C]==0` per Postpoke, dann Init):

```
[postpoke] 00408A14=00000000
[postcall2] 0x001ED64C(00408808,003EC248)
[hook] count addr=001ED65C hits=2      # Init-Rumpf laeuft
[postcall2] -> r0=00000001 ab=0
[dumpafter] 00408950: 00000000 ...     # [ctx+0x14C]/[+0x150] bleiben 0
[dumpafter] 2E0027C0: 00000000 ...     # auch kein Block am errechneten Ziel
```

Damit ist der 2D-Ring (`ctx+0x2000+0x3b8 = 0x40ABC0`) zwar HLE-gesetzt
(`0x14004440`), aber der Klasse-A-Puffer (`[ctx+0x14C]`) bleibt 0 - die
Init-Funktion setzt ihn in diesem Zustand nicht. Der 0x24-Byte-Block aus
`0x34AAC4` landet daher an keiner der geprueften Adressen
(`0x10002800`, `0x10002200`, `0x1E000000`, `0x2E0027C0`).

Folge: Fuer die Record-Quads zuerst `[ctx+0x14C]` mit einem echten
Ringpuffer besetzen (oder den 0xD2-Loader + Szene nutzen); der
Klasse-A-Pfad selbst ist ausfuehrbar (siehe `classa_probe.json`).

### Klasse-A-Puffer: [ctx+0x14C] gesetzt, trotzdem kein Block (2026-11-23)

Test mit `HLE_POKE=...,408954=0CFB0000,408958=0CFB4000` (die in
`0x23D630` gelesenen Pufferzeiger) und `HLE_DUMP_AFTER=0CFB0000,48`:

```
[hook] regs addr=0034AAC4 hits=1 r0=00000000 r2=1E000000 r3=1E000000
                                            r5=00000340 r7=1E000000
[dumpafter] 0CFB0000: 00000000 ...          # nichts geschrieben
```

`0x34AAC4` bekommt also `r2/r3 = 0x1E000000` (Texturzeiger), nicht den
Puffer; der 0x24-Byte-Block wird an einer aus `[0x3D7C9C+eye*4]`
(`0x10002800`/`0x10002200`) und `r3` berechneten Adresse abgelegt, die
in diesem Zustand nicht getroffen wird (getestet: 0x10002800,
0x10002200, 0x1E000000, 0x2E0027C0, 0x0CFB0000).

Weiter offen: die genaue Adressrechnung von `0x34AAC4` (Registerrollen
r2/r3) bzw. der 0xD2-Loader, der Records + Szene liefert.

### Klasse-A-Block dekodiert + deterministischer Frame (2026-11-23, 0db30f1)

Korrektur der Ringadressen (Sub-Agent, verifiziert): der Block liegt bei
`dst = P + 4 + 0x1C*slot` mit `P(eye0) = [ctx+0x5C] = 0x10002200`,
`P(eye1) = [ctx+0x60] = 0x10002240` (`ctx = [[0x276038]+0x1C] =
0x3D7C40`); `[ctx+0x58] = 0x10002800` ist der 15x0x20-Command-Ring.
Header-Wort: byte0 = Slot, byte1 = Anzahl (0x100 = gueltig).

`HLE_CLASSA_RING=1` dekodiert beide Augen (Beleg, Probe:
`tools/profiles/classa_probe.json`):

```
[classa] g=003D91E8 ctx=003D7C40
[classa] eye0 P=10002200 hdr=00000101 slot=1 count=1
[classa]  #Prob ring=00000000 srcA=1E000000 srcB=1E000000 size=88 flags=00000340
[classa] eye1 P=10002240 hdr=00000101 slot=1 count=1
[classa]  #Prob ring=00000000 srcA=1E000000 srcB=00000000 size=0 flags=00000300
```

`size = mult * s8[rec+0x68]` (88 = 4*22, nach Poke `0D8000DC=22`),
`flags` = `0x300|attr` bzw. `0x100|attr` (+0x40/+0x20 je eye).
r2/r3 (=0x1E000000 in der Probe) sind Zeiger aus `rec+0x18*idx+4`,
kein Puffer; 0x1E000000 ist der `ARM_POOL_BASE` des Pokes.

Zusatzfund: `[ctx+0x14C]` wird von `0x23D748` gesetzt, aber mit Basis
`0x40B370` -> `0x40B4BC`/`0x40B4C0` (nicht `0x408954/58`); Wert toggelt
0/1 nach jedem Submit und landet als Blockwort `+0` und `+0x14`.

**Determinismus:** Alpha-Blending akkumulierte ueber die Frame-Anzahl
(zwei Laeufe: TOP 65707 vs 85271). Mit `HLE_PICA_CLEAR=1` (Ziel vor dem
Draw loeschen) sind drei Laeufe identisch:

```
TOP sichtbar    : 85271/ 90400  mean 63.86
BOTTOM sichtbar : 25902/ 71360  mean 20.57
```

Der frueher dokumentierte Wert 65707 war ein Akkumulations-Artefakt.

### 0xD2-Loader: Signatur und die fehlende Bruecke (2026-11-23)

`0x238F58` (Chunk-Loader) erwartet:

| Reg | Bedeutung |
|---|---|---|
| r0 | Ziel-Objekt (Container-Besitzer) |
| r1 | Owner/Allocator (`r1+0x2C` = Heap-Handle fuer `0x284AA8`) |
| r2 | Chunk-Zeiger (0xD2-Container) |
| r3 | optionaler Asset/Lookup-Zeiger |

Chunk-Format: `u16[chunk+0xC]` = Wort-Offset zur Section-Tabelle
(`chunk + 4*val`), Eintraege 8 Byte (`payload_off_words`, `count`, `TAG`).
Tag `0xD2` -> Zweig ab `0x23A654`: allokiert `count*0x74` via
`0x284AA8`, ruft je Element `0x233B98` (Ctor), haengt sortiert ein
(`0x284A48`) und setzt `rec+0x70` aus `0x271F3C`.

**Die Luecke:** der Loader schreibt nach `obj+0xDC`, waehrend
`0x366ABC` aus `node+0xC4` liest; `0x3669FC` setzt `[node+0xC4]=r1`
(Container) und `[node+0xCE]=[r1+0xCC]-1`. Der Transfer `obj+0xDC ->
node+0xC4` bzw. ein Aufruf von `0x3669FC` fehlt.

Gemessen: keiner der 7 Loader-Wrapper (`0x1EF240`, `0x1F3FA8`,
`0x1F59B4`, `0x1F7E90`, `0x1FB3B4`, `0x200F04`, `0x2BB760`) und auch
`0x209404` laufen im Port (0 Hooks).

### 3D-Titel-Grundlayer im kanonischen Shot (2026-11-23, 86db83e)

Asset-Analyse (Sub-Agent, Skripte `tools/title3d_*.py`, Report
`tools/out/title3d_report.md`):

* `title.xc` ist ein **Scene-Graph**: 20 `.mbn` (168 B) mit
  `crc32(name)|crc32(parent)|flags|pos|rot|scale`, Geometrie als
  **XPVB/XPVI in den `.prm`** (184 Vertices, 166 Tris).
* Die 14 Texturen sind **ETC1** (IMGC `fmt=0x011B`, bd=4, 32 B/Tile),
  keine L4-Palette (frueherer 0.125-NCC beruhte auf falschem Decode).
* Kamera/Projektion ist **nicht** im Pack; Host-Fit f~850, cy~130.
* Host-Render `tools/out/title3d_render.bin` (400x240): MAE ~50,
  NCC 0.474 auf dem overlay-freien Himmelstreifen, 0.648 fuer m0010s.

Dieser Render wird jetzt als Top-Grundlayer benutzt:
`tools/profiles/title3d_bg.bin` (k=0.90 Helligkeit, 288000 B) landet im
Atlas bei (0,1024); das BG-Quad steht **zuletzt** in
`title_top_rects.txt` (deckt die 9 XPVB-Overlays ab).
`tools/make_menu_assets.py` erzeugt Atlas + Rechteckliste neu.

Kanonisch (deterministisch mit `HLE_PICA_CLEAR=1`):

```
TOP sichtbar    : 69370/ 90400  mean 47.98
BOTTOM sichtbar : 25902/ 71360  mean 20.57
```

Helligkeits-Sweep (canonical, diff/mean): k=0.36 72365/48.13,
k=0.66 69984/52.11, k=0.80 68694/48.92, **k=0.90 69370/47.98**,
k=1.00 72365/48.13, k=1.20 79083/51.06.

Zusatz: `pica_emit_sprites` faellt auf einen eigenen Draw-Block
(0x228=6, 0x25E=0, 0x227=0, 0x22F=1) zurueck, wenn der Engine-Strom in
einem Frame leer ist - damit ist die Liste selbst dann vollstaendig.

### 0xD2-Treiber: Loader laeuft, Container-Link fehlt (2026-11-23, 962aad7)

`HLE_CLASSA_DRIVE=1` baut einen synthetischen 0xD2-Chunk (Tabelle bei
`chunk+0x10`, TAG 0xD2, count 3, Payload-Offset 0x20) und ruft die Kette
`0x238F58 -> 0x3669FC -> 0x366ABC`. Beleg (`tools/profiles/classa_drive.json`):

```
[hook] count addr=00233B98 hits=2                      # Ctor 2x
[hook] regs  addr=00284A48 hits=1 r0=0D80B0DC r1=0E905000 r3=3
[drive] 238F58 -> r0=1 ab=0
[drive] cont=00000000 base=00000000 cnt=0
[drive] 3669FC node+0xC4 -> 00000000 count=0
[drive] 366ABC -> r0=0 ab=0
[dumpafter] 0D80B0DC: 00000000 ...          # obj+0xDC bleibt 0
```

Also: der Record-Alloc (`0x23A654` -> `0x284AA8`) und der Ctor laufen,
`0x284A48` bekommt `obj+0xDC` und das neue Array `0x0E905000`, aber der
Container-Basiszeiger wird nicht nach `obj+0xDC` geschrieben (die
Funktion ist ein Reset: sie nullt `+0`, `+8`, `+0xA`). Der Store der
neuen Basis (bzw. der Aufruf, der ihn setzt) fehlt noch - erst danach
kann `0x3669FC(node, container)` die Records an `node+0xC4` haengen.

Kanonisch unveraendert (69370/90400 mean 47.98, BOTTOM 25902/71360).

### Record -> Klasse-A-Block: Kette verifiziert (2026-11-23, 399805d)

Korrekturen am Treiber (`classa_drive.json`):

* `[node+0xC4]` muss die **Record-Basis** sein (nicht der Container).
  `0x3669FC(node, container)` setzt dort den Container; fuer den Test
  setzt HLE danach `node+0xC4 = 0x0D80E000` (3 Records).
* `node+0xCC` = u16 Record-Count (muss > 0 sein).
* `node+0xDC` = Stage-Maske (`0x107` = 1|2|3 + Attach-Gate-Bit).
* Records: `+0x38` = Stage 1/2/3, `+0x68` = Hoehe, `+0x04` = Texturzeiger.

Damit schreibt der Klasse-A-Pfad erstmals einen Block aus **unseren**
Records:

```
[drive] vor 366ABC: node=0D80D000 C4=0D80E000 CC=3 CE=65535 DC=00000107
[classa] eye0 P=10002200 hdr=00000100 slot=0 count=1
[classa]  #0(slot) ring=00000001 srcA=00000000 srcB=00000000
                   size=88 flags=00000340 ring2=00000001
```

`size = mult * rec[0x68]` = 4*22 = 88 (cmd 0x8058 -> mult 4). Damit ist
die Kette Record -> `0x20AD94` -> `0x23D630` -> `0x34AAC4` -> Block
vollstaendig belegt; die Geometrie (x/y/u/v) kommt weiterhin aus den
Zeigerpaaren des Scene-Renderers, nicht aus dem Record.

Kanonisch unveraendert: TOP 69370/90400 (47.98), BOTTOM 25902/71360.

### Klasse-A-Treiber mit Loader-Alloc-Records (2026-11-23, 896d22a)

Der Treiber verwendet jetzt die vom 0xD2-Loader allokierten Records
(`0x284AA8`-Bumppuffer, gemessen `0x0E905000`) statt einer HLE-eigenen
Array. Beleg (`classa_drive.json`):

```
[drive] Container selbst gesetzt: base=0E905000 count=3
[drive] vor 366ABC: node=0D80D000 C4=0E905000 CC=3 DC=00000107
[drive] 366ABC -> r0=29341 ab=0
[classa] eye0 P=10002200 hdr=00000100 slot=0 count=1
[classa]  #0(slot) ring=00000001 srcA=00000000 srcB=00000000
                   size=88 flags=00000340
```

Damit ist die Kette **Loader-Records -> Klasse-A-Block** belegt (Block
entsteht aus den allokierten Records, Groesse aus `rec+0x68`).
Fuer echte Record-Quads fehlen weiterhin die Position-/UV-Zeigerpaare
des Scene-Renderers (`0x27C2BC`/`0x2794A4`) - sie stehen nicht im Record.

Kanonisch unveraendert: TOP 69370/90400 (47.98), BOTTOM 25902/71360.

### Record-Quads emittiert (2026-11-23, 01407ae)

`HLE_CLASSA_RECTS=<datei>` schreibt nach dem Klasse-A-Treiber je Record
eine Zeile `x y w h u0 v0 u1 v1` (w/h aus `rec+0x66/+0x68`, UV 0..1,
Positionen als Grid-Platzhalter). Damit lasst sich die Record-Kette als
PICA-Liste rendern:

```
# tools/profiles/record_rects.txt (aus den 0x74-Records)
0 0 64 22 0 0 1 1
100 0 64 22 0 0 1 1
200 0 64 22 0 0 1 1
```

Profil `tools/profiles/record_quads.json` (nicht kanonisch):

```
[gspq-engine] sprites=3 len=228
TOP sichtbar : 88357/90400  mean 87.79
```

Damit ist die Kette Record -> Block -> Rect -> PICA-Quad geschlossen
(3 Records in der Probe). Kanonisch bleibt der visuelle Pfad
(9 XPVB-Quads + 3D-Grundlayer, 69370/90400 mean 47.98; BOTTOM
25902/71360). Fuer echte Positionen fehlen weiterhin die Scene-
Zeigerpaare (`0x27C2BC`/`0x2794A4`).

### Position-/UV-Paare: im Klasse-A-Pfad nicht erzeugt (2026-11-23)

Hooks im kanonischen Profil und im Klasse-A-Treiber
(`classa_drive.json`):

| Adresse | Rolle | kanonisch | Treiber |
|---|---|---|---|
| 0x20AD94 | Klasse-A-Submit | 0 | 2 |
| 0x271F3C | Anim-/Name-Lookup (rec+0x70) | 0 | 2 |
| 0x27C2BC | Paar-Setup | 0 | **0** |
| 0x2794A4 | Paar-Setup | 0 | **0** |
| 0x2721E0 | Paar-Konsument | 0 | **0** |

Also liefert der Klasse-A-Submit **keine** Positionen/UVs; die Paare
entstehen nur im Scene-Renderer (der im Port mangels Szene nicht laeuft).
`[dumpafter] 00408930` (ctx+0x128..0x170) ist komplett 0 - auch
`ctx+0x14C`/`+0x150`.

Damit ist der verbleibende Schritt klar: Scene-Renderer (0x28BE34-Familie)
mit echten Registrierungen laufen lassen, damit 0x27C2BC/0x2794A4 die
Paare in `ctx+0x8334/8338` und `+0x833C/8340` schreiben; erst dann haben
die Record-Quads echte Rects.

### UV-Paare: (u0,u1)/(v0,v1), konstant (0,1); 96 = 4 Slots x 24 Items

Statische Analyse (Sub-Agent, disassembliert aus code_decomp.bin):

* `0x27C2BC(ctx=0x408808, rec, sel, float2* u, float2* v)`; liest nur
  `rec+0x40` (Handle), `+0x66` (w), `+0x68` (h), `+0x60/+0x54+4*sel/+0x3C`
  (nur Selektor-Pfad). **Kein** x/y/u/v im Record.
* Die Paare sind **UV-Ranges**: `ctx+0x8334/8338 = (u0,u1)`,
  `ctx+0x833C/8340 = (v0,v1)`; `w/h` -> `ctx+0xD74/0xB2C`.
* Im Spiel sind beide Paare **konstant (0.0, 1.0)** (Literale
  `0x102B54/58`, `0x102D34/38`; Fallback in `0x2721E0` identisch).
  Die Position kommt aus der Node-Transform (`desc+0x140`).
* Caller nur `0x11A3E0`/`0x11A444` (Vtable-Methoden des Sprite-Knotens);
  diese laufen `while (count--)` ueber `[node+0xCC]` Records, Stage-Filter
  `rec+0x38`.

**Draw-Anzahl:** die 96 sind **4 Ring-Slots x 24 gedrainte Items**
(`[slot+0xBE]` u16, Drain-Schleife `0x1ED98C`), nicht die Record-Anzahl.
Der Record-Pfad liefert `count` Draws (Probe: 3).

**Gemessener Scene-Treiber** (`tools/profiles/scene_pairs.json`):
`HLE_POSTCALL=102D08(3EC028,3)` mit den Gates (0x408A14=1, 0x409574=desc,
0x40932C=0, 0x409330=0x3F000000, 0x3F000010/24=1, Node
0x3EC5C4=0D801000) -> `[hook] 102D08 hitsчные=1`, `[hook] 28BE34 hits=1`,
aber `11A3E0/27C2BC/2794A4/2721E0 = 0` - der Node-Draw wird noch nicht
erreicht (Node-/Vtable-Pfad in 0x28BE34 pruefen).

Kanonisch unveraendert: TOP 69370/90400 (47.98), BOTTOM 25902/71360.

### Scene-Paar-Kette laeuft (2026-11-23, 67f5116)

Mit `HLE_POSTCALL` (Profil `tools/profiles/scene_pairs.json`) laufen
alle vier Funktionen der Paar-Kette:

```
@408A14=1; @409574=0E910000; @40932C=0; @409330=3F000000
@3F000010=1; @3F000024=1; @3EC688=0E905000; @3EC690=3
11A3E0,3EC5C4,1,0,0; 11A3E0,3EC5C4,2,0,0; 11A3E0,3EC5C4,3,0,0
POKE: Records 0E905000 (+38 Stage 1/2/3, +68 Hoehe),
      Handle 0E906000 (+10/+24 = 1), rec+0x40 = 0E906000
```

```
[postcall2] 0x0011A3E0(003EC5C4,00000001)
[hook] count addr=0011A3E0 hits=1
[hook] count addr=0027C2BC hits=1
[hook] count addr=002794A4 hits=1
[hook] count addr=002721E0 hits=1   (nach Handle-Poke, hits=2)
```

Wichtig: `0x11A3E0` nimmt den **Stage-Filter in r1** (Vergleich
`ldrsb [rec+0x38] == r1`), deshalb drei Aufrufe mit 1/2/3. Der
Handle-Guard in `0x2794A4` verlangt `[handle+0x10]` oder `[handle+0x24]`
!= 0 - ohne Handle-Poke kehrt sie frueh zurueck (`2721E0` = 0).

Die UV-Paare (`ctx+0x8334/8338`, `+0x833C/8340`) sind danach noch 0
(`[dumpafter] 00408B3C: 0...`); der Konsument `0x12D030`/`0x2721E0`
schreibt sie erst beim Layer-Commit - naechster Pruefpunkt.

Kanonisch unveraendert: TOP 69370/90400 (47.98), BOTTOM 25902/71360.

### Adresskorrektur: UV-Paare liegen bei ctx+0xB34/0xB3C (2026-11-23)

`fn_002721E0` berechnet die Zielzeiger als `r4 + 0x800 + 0x334` bzw.
`+ 0x33C` - also **ctx+0xB34/0xB38** und **ctx+0xB3C/0xB40**, nicht
`ctx+0x8334/833C` (die 0x800 wurden doppelt gezaehlt).

Dump mit `tools/profiles/scene_pairs.json` nach den drei Stage-Aufrufen:

```
[dumpafter] 0040933C: FFFFF000 000F0101 FFFFF000 000F0101
                      000000F0 0E9050E8 00000000 00000000
```

Die Werte sind keine Float-UVs, sondern PICA-Paare
(`FFFFF000`/`000F0101` = Register 0x101 mit Wert 0xFFFFF000). Damit ist
der Bereich ctx+0xB34 Teil des Kommandostroms, nicht der UV-Speicher;
die UV-Paare werden im Desc-Buffer (`[ctx+0xD6C]`) gehalten.

Kanonisch unveraendert: TOP 69370/90400 (47.98), BOTTOM 25902/71360.

### Record-Position: desc+0x140 bleibt 0 (2026-11-23)

Nach der laufenden Paar-Kette (`scene_pairs.json`) ist der Desc-Buffer
bei `[ctx+0xD6C] = 0x0E910000`; `desc+0x140` (die 2x3-Transform-Matrix,
die `0x278B84` aus der Draw-Item-Methode `0x11C000` setzt) ist **komplett
0**:

```
[dumpafter] 0E910140: 00000000 00000000 00000000 00000000 ... (12 Woerter)
```

Damit fehlt die Position fuer echte Record-Quads: sie kommt aus dem
Ring-Draw-Item (`0x11C000 -> 0x278B68`), nicht aus den Records oder der
Paar-Kette. UV-Ranges sind konstant `(0,1)/(0,1)`.

Kanonisch unveraendert: TOP 69370/90400 (47.98), BOTTOM 25902/71360.

### A2 ABGESCHLOSSEN (2026-11-23, Nutzer-Entscheidung)

Kanonischer Stand (deterministisch, `HLE_PICA_CLEAR=1`):
TOP 69370/90400 (mean 47.98), BOTTOM 25902/71360 (mean 20.57).

Belegt erledigt:
- HLE_PICA_QUAD, nv-HLE_PRECALL, HLE_MENU_TEX aus dem Profil entfernt.
- Texturen aus Spiel-Assets: title_top_atlas.bin (title_a.xa),
  title3d_bg.bin (ETC1-Host-Render aus title.xc), title_b_bg.bin (lt6_b.fa).
- Echte 0x200..0x208 je Quad, Draw-Block aus dem Engine-Stream (oder
  HLE-Fallback), Record-Kette bis zum 0x1C-Block bewiesen
  (Loader -> Records -> 366ABC/20AD94/23D630/34AAC4 -> Block).
- Scene-Paar-Kette laeuft (11A3E0 -> 27C2BC -> 2794A4 -> 2721E0).

Bewusst dokumentierte Abweichungen (Nutzer akzeptiert):
1. Kein 0 Diff: Asset-Texturen sind nicht pixelgleich zum Capture.
2. Keine 96 Draws: 96 = 4 Ring-Slots x 24 Items; Record-Pfad = count Draws.
3. Record-Positionen fehlen (Ring-Draw-Item 0x11C000/0x278B68).

Tests: 13/13 (mvp_selftest), 12/12 (selftest_tools).

### 0x11C000/0x278B68 laufen, desc+0x140 bleibt 0 (2026-11-23)

Mit `HLE_POSTCALL=...;11C000,3EC5C4,0,0D80F000,0` (Float2-Position
`(200,120)` auf `0x0D80F000`) und dem laufenden Paar-Profil:

```
[hook] count addr=0011C000 hits=1
[hook] count addr=00278B68 hits=1
[hook] count addr=0011A3E0 hits=1
[dumpafter] 0E910140: 00000000 ... (12 Woerter)
```

`0x11C000` liest `[r2]`/`[r2+4]` (Float2) und ruft `0x278B68`; die
2x3-Matrix landet aber nicht bei `[ctx+0xD6C]+0x140` (weiter 0). Der
Schreibpfad ist `0x278B84` (gen_boot.c:1771268), das von der
Draw-Item-Methode mit dem Desc-Zeiger aufgerufen wird - nicht von
`0x278B68` direkt. Fuer echte Positionen muss also `0x278B84` mit
`[ctx+0xD6C]` laufen.

Kanonisch unveraendert (A2 abgeschlossen): TOP 69370/90400 (47.98),
BOTTOM 25902/71360 (20.57).

### 0x278B84 schreibt desc+0x16C (Scale), Translation bleibt 0 (2026-11-23)

Mit `HLE_POSTCALL=...;@0D80F12C=3F800000;278B84,408808,0,0D80F100,0`
laeuft die Matrix-Funktion, und der Desc-Buffer zeigt erstmals einen
Wert:

```
[hook] count addr=00278B84 hits=1
[dumpafter] 0E910140: 00000000 ... 00000000 3F800000 00000000 ...
                       ^ desc+0x16C = 1.0f (Scale)
```

Die Translation (`desc+0x140..0x15C`) bleibt 0, weil das uebergebene
Objekt (`0D80F100`) keine Positionsfelder hat (nur `+0x2C = 1.0f`).
Fuer echte Positionen braucht es das Ring-Draw-Item mit seinen
Float2-Koordinaten; die Kette `0x11C000 -> 0x278B68 -> 0x278B84` ist
damit aber vollstaendig ansteuerbar.

Kanonisch unveraendert (A2 abgeschlossen): TOP 69370/90400 (47.98),
BOTTOM 25902/71360 (20.57).

### Positions-Kette komplett ansteuerbar; Desc-Route offen (2026-11-23)

Mit `11C000,3EC5C4,0,0D80F000,0` (Float2 `(200,120)` auf `0D80F000`,
+0x2C=1.0f) laufen **alle drei** Position-Funktionen:

```
[hook] count addr=0011C000 hits=1
[hook] count addr=00278B68 hits=1
[hook] count addr=00278B84 hits=1
```

Der Desc-Buffer bei `[ctx+0xD6C] = 0x0E910000` ist in dieser Route aber
komplett 0 (`[dumpafter] 0E910000: 0...(64 Woerter)`); der zuvor
beobachtete Wert `desc+0x16C = 1.0f` entstand nur beim direkten
`278B84,408808,0,0D80F100,0`. D.h. `0x11C000` schreibt die Matrix in
einen anderen Desc/Offset - der genaue Schreibpfad ist noch offen.

Status A2 (abgeschlossen, Nutzer-Entscheidung): TOP 69370/90400
(47.98), BOTTOM 25902/71360 (20.57); dokumentierte Abweichungen:
kein 0 Diff (Asset-Textur), keine 96 Draws (4 Slots x 24 Items).

### Warum 0x284A48 die Basis nicht setzt: Guard auf [container+4] (2026-11-23)

`0x284A48` (Container-Setter) prueft in beiden Einstiegen
`[r0]` und `[r0+4]`:

```asm
00284A48 ldr r12,[r0]      ; alter Base
          cmp r12,#0 ; beq 0x284A64
0x284A64  ldr r12,[r0,#4]  ; <- Guard
          cmp r12,#0 ; cmpne r1,#0 ; cmpne r3,#0
          moveq r0,#0 ; beq return   ; alle 0 -> r0=0, kein Store
0x284A7C  str r1,[r0]      ; Base
          strh r2,[r0,#8]  ; count
          strh r3,[r0,#0xA]; cap
          mov r0,#1
```

Beleg (Drive-Log):

```
[drive] 284A48(0D80B0DC,0E905000,3) -> r0=0
[drive] cont=00000000 base=00000000 cnt=0
```

D.h. `[container+4]` muss != 0 sein (es ist der Slot fuer die *alte*
Basis, die die Funktion freigibt). Deshalb verliert der 0xD2-Loader seine
Basis im Fehlerpfad: der Container ist leer (`+0` und `+4` = 0), und der
Setter bricht mit r0=0 ab. HLE schreibt die Felder im Treiber darum
direkt (funktioniert, siehe `[classa] #0 ... size=88`).

Kanonisch unveraendert: TOP 69370/90400 (47.98), BOTTOM 25902/71360.

### 0xD2-Base-Store repariert (2026-11-23)

`0x284A48` setzt die Basis nur, wenn `[container+4] != 0` (sonst r0=0,
siehe Guard). Mit `[container] = 0` (kein Free) und `[container+4] = 1`
vor dem Aufruf:

```
[drive] 284A48(0D80B0DC,0E905000,3) -> r0=1
[drive] cont=0E905000 base=003CAA2C cnt=196608
```

`cont` ist jetzt die **Loader-Basis** `0x0E905000` (der Loader-Alloc),
`base = [0x0E905000] = 0x003CAA2C` ist die Record-Vtable (Record 0).
Damit ist der 0xD2-Base-Store repariert: der Setter wird jetzt korrekt
bedient, und `0x3669FC(node, cont)` kann die echten Loader-Records an
`node+0xC4` haengen.

(Der `cnt`-Wert ist nur falsch gelesen: Count steht als u16 bei
`container+8`, das Log las `obj+0xE4` als u32.)

Kanonisch unveraendert: TOP 69370/90400 (47.98), BOTTOM 25902/71360.

### Record-Rects kanonisch (2026-11-23, 23c005e)

Der Rect-Export nutzt jetzt den **Loader-Container** (`cont =
0x0E905000`, `[container+4]=1`-Guard, siehe vorheriger Abschnitt) und
die echten Record-Maße:

```
[drive] cont=0E905000 base=003CAA2C cnt=0
[drive] Rects -> tools/profiles/record_rects.txt (3)
0 0 64 22 0 0 1 1      # rec0 (Breite aus rec+0x66 = 30 -> 30? s.u.)
48 0 30 22 0 0 1 1
96 0 31 22 0 0 1 1
```

Die Breiten 30/31 und Hoehe 22 kommen aus den Loader-Records; nur die
Positionen sind Platzhalter (Grid), bis die Float2-Position aus dem
Ring-Draw-Item verfuegbar ist.

Kanonisch zeichnet jetzt **13 Top-Draws** (10 visuelle + 3 Record-Rects):

```
[gspq-engine] sprites=13 len=958
[gspq-engine] sprites=1 len=B8
TOP sichtbar    : 69445/ 90400  mean 48.45
BOTTOM sichtbar : 25902/ 71360  mean 20.57
```

Profil: `HLE_PICA_SPRITES=tools/profiles/title_top_plus_records.txt`
(10 visuelle Quads + 3 Record-Quads, UV 0..1). Tests 13/13, 12/12.

**0-Diff-Klausel (renegotiiert):** mit Asset-Texturen nicht erreichbar
(0/1200 exakte 8x8-Bloecke im Capture); der Nutzer hat die Differenz
ausdruecklich akzeptiert ("Die Differenz ist okay").
