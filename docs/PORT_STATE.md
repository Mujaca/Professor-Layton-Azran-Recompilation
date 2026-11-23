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
`0x1BA364` gar nicht — er laeuft auf einem anderen Pfad (Modus-/State-
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
`0x3D7C40` in `+0x1C` — wie im echten Boot.

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
Shared-Memory-Zeiger) — `[gspengine] shm(base+0x2C)=00000005`
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
(unsere Engine-Methoden lesen z. B. `[r0+0xA34]`) — der Ring-Eintrag ist
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
Sprite-/Mesh-Renderpfad — die echten Drawable-Objekte (Sprites) fehlen
weiterhin.

### Write-Watch: das Spiel fuellt den Drawable-Pool nie

`ARM_MEMWATCHRANGE=3EE188,20` im Menue-Loop ohne `HLE_DRAW_LIST`
(`out/logs/poolw_log.txt`): nur BSS-Clear (`pc=0x100030`) und der
Konstruktor (`pc=0x288398`) schreiben in den Pool. **Keine** Spiel-
Schreibzugriffe auf `{array,stride,start,end}` (`menu+0x5C..`) waehrend
120 Frames — der Menue-Loop erzeugt also selbst keine Drawables. Das
erklaert `sets=0` fuer alle Kandidaten-Objekte.

Statische Suche nach dem Add-Writer (`tools/_find_pool_writers.py`):
569 Treffer fuer `+0x5C`, 27 fuer `+0x6A` — viele davon sind aber
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
`menu+0x38` — Menue-Node-Liste **und** Objektliste sind im Port leer.
Damit ist klar: Nicht der Renderer, sondern die **Befuellung des
Menue-Objekts** fehlt.

Das Spiel ruft `0x28D424` selbst mit `r0=0x003EE12C` auf
(`ARM_WATCH=28D424`, `out/logs/w28_log.txt`); ein Zeiger auf
`0x3EE12C` existiert aber nirgends im RAM (`[menuscan] ptr(3EE12C):`
leer) — die Adresse wird berechnet (Basis `0x3EC028` + Offset).

**Aufrufkette des Menue-Loops:**

```
fn_0011DB84(r0=Menue)  ->  fn_00277004 (Menue-Drain: 0x28D424/0x2800C8)
fn_0035FF14 (Scene-Builder, kein statischer Aufrufer)
    0x3610C0..0x3610D8  bl 0x265DD0   ; Container-Traversal
    0x3610E8  sub r4,r0,#0x80         ; r4 = &menu (eingebettetes Member)
    0x3610F8  bl 0x277004
```

`0x3C6FE4+8` ist genau `fn_0011DB84` — das State-Objekt ruft also den
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
0x0028EF94)` ab — das echte World-Objekt fehlt (Menue-Flow nicht erreicht).

## Offline-IMGC-Dekoder ist fuer den Menue-Atlas falsch

`out/x_menu/000.xi` (aus `s_menu_ani_a.xa`) enthaelt drei IMGCs; das
512x256-24bpp-Bild hat eine Tabelle mit **2048 eindeutigen** Eintraegen der
Form `block*4096 + row*16 + col` (`block` 0..7, `row`/`col` 0..15). Der
Kuriimu-Ansatz `offset = entry * bytesPerTile` liefert deshalb **schwarz**
(`_imgc.decode`), und auch die Permutationsvarianten
(`s = (e>>12)*256 + (e&0xFFF)`) ergeben nur Rauschen (Korrelation 0.01–0.11).
Der entpackte Texturstrom selbst ist ein Farbverlauf (Korrelation 0.08).

Damit sind die frueheren Aussagen "kein Asset matcht die Referenz" **nicht
belastbar** – sie beruhen auf einem falschen Dekoder. Werkzeuge:
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
  - `tools/_romfs_fullchain.py`  — alle File-Buckets -> vollstaendige Kette
  - `tools/_romfs_dirchain.py`   — alle Dir-Buckets -> vollstaendige Kette
  - `tools/_romfs_rootbucket.py` — Bucket 0 -> Root-Record 0
  - `tools/_patch_lookup_noparent.py` — Parent-/Laengenpruefung neutralisieren
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

Zusatzmessung: der Menue-Zustand loest noch **keine** Archiv-Lookups aus —
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
kein `[gspq]`. Das Spiel fuellt den Kommandopuffer also gar nicht — passend
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
  (`pc=0x288398`, `lr=0x2BA728`) — der Loader setzt das Flag nie auf 1.
  In 90 s läuft die Schleife ~6400×, `0x101540` wird nie erreicht.
- Ein `STR [Rn,#0x2144]` existiert im Image nicht (auch kein `ADD #0x2144`);
  gefunden wurde nur **ein** Paar `ADD #0x2000` + `ADD #0x144`
  (`0x115D8C`/`0x115D90`, `fn_00115C18`) — dort wird das 0x200-Byte-Objekt
  bei `scene+0x2144` per `0x28EED0` **genullt** (Reset, nicht Setter).
  Zusätzlich 20 Stellen `add #0x2000` + `STR [.,#0x144]` (u. a. `0x21B408`,
  `0x2FE1CC`) — Kandidaten für den Setter, aber Index-/Array-Stores.
- Es gibt **keinen** gespeicherten Zeiger auf `0x3EE16C` im Scene-Bereich
  (Dump 0x3EC028..0x3EE200, kein Wert `003EE16C`) — der Setter muss die
  Adresse selbst bilden.
- `.fa`-Dateien werden **nicht über FS** geöffnet: `HLE_FS_PATHS` zeigt nur
  SelfNCCH, `OpenArchive id=7`, `/gamecoin.dat` und drei `file 0x0802`-Reads.
  Die `.fa`-Geräte bekommen also entweder leere Synthese-Dateien oder werden
  gar nicht geöffnet.
- Die `0x0802`-Reads tragen **Binärpfade** (Azahar: `NCCHFilePath`, 0x14 Byte:
  `{open_type, content_index, filepath_type, exefs_filepath[8]}`) — unser
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
  erstmals `0x124AD8` (hinter `0x12FD30`) — vorher brach er bei `0x124A28` ab.
  `0x12FD30` selbst liefert 0 (Abbruch ueber `beq 0x124C30`); mit
  `0x12FD30 = mov r0,#1` laeuft der Lader weiter, erreicht `0x124C30` aber
  nicht in 120 s (Hauptthread idle bei `0x283488`, `sp=0x0CF0F000`).
  `HLE_FLAG_FIX` (Byte `0x3E7771`, gelesen von `0x139844`) aenderte daran
  nichts: der Wert ist bereits `1` (Wort `0x3E7770 = 0x00010100`), `0x139844`
  liefert also 1 — der 0-Rueckweg von `0x12FD30` kommt aus einem spaeteren
  Zweig. Auch mit Stub (0x12FD30 = mov r0,#1) bleibt der Lader nach `0x124AD8` in 240 s stecken
  (`0x124C30` wird nicht erreicht, Watchdog `0x283488`).
- Mit dem kurzen Pfad (`0x1249CC → 0x124C30`) bleibt der Mount-Erfolg
  reproduzierbar.
- `boss:U`, `/head.lt6`, `SetBufferSwap (0x0005)` und ein echter Frame fehlen.

### Screenshot-Timing (wichtig!)

`tests/port.c` speicherte `--shot` frueher bei **Frame 3** (~48 ms) und brach
bei Frame 6 ab. Das Spiel hat zu diesem Zeitpunkt noch nichts gerendert —
alle bisherigen "288.054 B, 0 Unterschiede"-Befunde waren dadurch
vorbestimmt. Neu: `PORT_SHOT_FRAMES=<n>` legt den Aufnahmezeitpunkt fest
(Default 3). Beleg mit `HLE_GPU_SUBMIT=1` (synthetischer Frame):

```
PORT_SHOT_FRAMES=200 -> shot_late.bmp: 288054 B, Unterschiede: 284003
```

Damit ist bewiesen: die VRAM-Pixel werden korrekt in den Screenshot
uebernommen, sobald sie existieren. Ohne Fremd-Submit (nur Spiel-Pixel)
bleibt es bei 0 Unterschieden — das Spiel schreibt weiterhin nichts in den
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
("unbekanntes call-Ziel 0x000000F0 von Block 0x001292BC") — ein korrupter
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

Damit ist klar, wie der Builder aufgerufen wird: als **Vtable-Methode bei +0x04** eines Objekts, dessen `[0] = 0x3CA370` ist. Im Port zeigt die Scene (`0x3CA494`) aber `[0]=0x003653CC` — also eine andere/aeltere Klasse; ein Aufruf von +0x04 der 0x3CA370-Tabelle wurde nie beobachtet (`ARM_WATCH=3610F8`: 0).

**Vtable-Test:** `HLE_SCENEBUILD=1 HLE_SB_ARG=3CA494` setzt zusaetzlich `[scene]=0x3CA370` (Klassen-Vtable) und ruft den Builder. Ergebnis (`out/logs/vt2_log.txt`): weiterhin 200x `unbekanntes call-Ziel` (0xE5900008/0), keine Engine-`SetCommandList` (192, alle `lr=001ED9C8`), `units=0`. Der Builder braucht also weitere initialisierte Sub-Objekte, nicht nur die Vtable.

**Stack-Areal getestet:** Builder-Aufruf mit eigenem grossen Stack (`SP=0x0D840000`) aendert nichts: weiterhin 200x `unbekanntes call-Ziel`, davon `0xE5900008` (Block `0x28EF94`). `0xE5900008` ist eine ARM-Instruktion (`ldr r0,[r0,#8]`), d. h. ein Feld des Scene-Objekts wurde als **Vtable-/Funktionszeiger** interpretiert und zeigte in den Code. Die vom Forced-Ctor (`0x381CE4`) gebaute Scene hat also eine andere/partielle Layout-Ordnung als die erwartete Klassen-Struktur.

**Frisches Scene-Objekt getestet:** `HLE_SB_NEW=1` legt ein genulltes 0x4000-Byte-Objekt bei `0x0D200000` an, setzt die Klassen-Vtable `0x3CA370` und ruft den Builder. Ergebnis (`out/logs/sbnew_log.txt`): weiterhin 200x `unbekanntes call-Ziel` (jetzt 0x0), keine Engine-`SetCommandList`, `units=0`, `menu`-Listen leer. Der Builder setzt also bereits initialisierte Sub-Objekte voraus.

**`0x265DD0` ist ein Clear-/Teardown-Helfer**, kein Container-Get: `ldr r1,[r0]; ...; str r1,[r0]; strh r1,[r0,#0xa]; strh r1,[r0,#8]; bx lr` — er nullt `[r0]`, `[r0+8]`, `[r0+0xa]` und laesst `r0` unveraendert. Die Kette `bl 0x265DD0; sub r0,r0,#0xc; ...` im Aufrufer `0x3610xx` ist damit eher ein Abbau/Pop einer Member-Kette als ein Aufbau. Die eigentliche Menue-Konstruktion liegt also woanders.

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

`fn_00100F98` springt also bei `0x101030` in den Fail-Pfad `0x1014E4`, weil der erste Loader `fn_001036B4` 0 liefert. Dessen erster Check ist `fn_0010FC9C` (liest `*(s8*)0x3D8CE0`); ein dauerhaftes Setzen von `[0x3D8CE0]=1]` (HLE_PUMP bei `HLE_ENGINE_MENU`) aendert nichts — der Fehlschlag liegt also tiefer in `0x1036B4` (`0x1036C8`, `0x103724` oder Folgeschritte).

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

**Nach erfolgreichem Init:** Write-Watch auf `0x3EE198`/`0x3EE1A0` zeigt weiterhin nur BSS/Ctor (`out/logs/c1_log.txt`, `c2_log.txt`). Ein zusaetzlicher Welt-Update-Aufruf `0x10F0B4(0x3EC028)` (`HLE_FRAME_LOOP2`) endet wieder in Null-Calls (`0x28EF94`), `units=0`, keine Engine-`SetCommandList` (`out/logs/wu_log.txt`). Auch ein erfolgreicher Menue-Init legt im Port also keine Nodes an — die Inhalte kommen offenbar erst aus spaeteren Zustands-/Sequenzschritten (Eingaben/Events).

**Frame-Callback 0x101750 laeuft:** `ARM_WATCH=101750` zeigt 51 Treffer in 120 Frames (r0=0x400, r1=0x101750). Der Callback ist ein Thunk `-> 0x103C18 -> 0x10FFA8`, das die Submodule `0x28E40C`, `0x11AF00`, `0x11B02C`, `0x121F3C` updated. Trotzdem bleiben `menu+0x38/+0x6C` leer — die Node-Erzeugung liegt tiefer in diesen Submodulen bzw. ist event-/eingabeabhaengig.

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

Davor/dazwischen stehen Code-Zeiger (`0x104420`, `0x104428`, `0x104440`, `0x1045E0`, `0x1045E8`, `0x10470C`, `0x1047DC`, `0x1047E4`, ...) — offenbar eine Tabelle von Menue-Seiten mit Handlern und Sequenznamen.

**Test:** Diese Handler (`10470C/1047DC/1047E4/104420/104428/104440`) per `HLE_FRAME_LOOP2` mit `r0=0x3EE12C` aufgerufen (`tools/_seq_matrix.py`): keine `.xq`-Datei wird geoeffnet, `menu+0x74` bleibt 0. Der eigentliche Sequenz-Loader ist also nicht darunter.

**MOVW/MOVT-Scan:** `tools/_movwt.py` sucht nach gepaarten MOVW/MOVT, die eine Adresse in `0x28FF90..0x290050` bilden. Ergebnis: **0 Kandidaten** (beide Rd gleich, Abstand <= 80 B). Die Menue-Namenstabelle wird also weder per Literal noch per MOVW/MOVT adressiert; sie ist vermutlich reine Debug-/Animations-Benennung oder wird ueber eine Laufzeit-Basis (Zeiger in .data) erreicht.

**FA-Read-Pfad im Menue nicht aktiv:** Hook auf den FA-Stream-Leser `0x280744` (Build-Patch fuer `hle_fa_read`) zeigt im Menue-Lauf **0 Treffer**; nur `0x28F730` (Sync-Init) wird 2x gerufen (`out/logs/fa_log.txt`). Es werden also gar keine `.fa`-Streams gelesen. Zudem injiziert der Port bereits HID-Eingaben (START/A alternierend alle 30 Frames, `runtime/hle/hle.c` Zeile ~3386) — Eingaben sind also nicht der Blocker.

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
`+0x18` bleibt 0 — daher die degenerierten Parameter.

Die eigentlichen Trigger-Wrapper (`cmd 0x000C`, normal=1) sind:
`0x13E3E0`, `0x3418BC`, `0x341D34` (`out/logs/tr*.txt`).
`ARM_WATCH` auf alle drei im Komplettlauf: **0 Treffer** — die Engine
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

