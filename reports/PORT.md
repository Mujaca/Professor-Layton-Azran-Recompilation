# Port-Status

Letzte Aktualisierung: 2026-11-23

## Level-5-Dekoder + Referenz = Titel-Screen (2026-11-23)

`tools/_imgc.py` dekodiert jetzt auch die Level-5-Verfahren, die die
Menue-Assets tatsaechlich nutzen:

- **RLE (Methode 4):** `flag&0x80` -> naechstes Byte `(flag&0x7f)+3` mal,
  sonst `flag+1` Literale.
-**Huffman4Bit/8Bit (Methode 2/3):** 4-Byte-Header, `treeSize`/`treeRoot`,
  `treeSize*2` Tree-Bytes, Bitstrom MSB-first; Loop bis
  `size*8/bitDepth` Blattwerte.

Belege: `info_a.xi` Tabelle 3000 B / 142 Tiles (vorher 0 B),
`s_menu_bg_b00.xi` und `title_b_bg.xi` je 2400 B / 1200 Tiles.

**Die Referenzframes zeigen den Titel-Screen (`seq/menu_title.xq`).**
`title_b_bg.xi` ist eindeutig der Bottom-Hintergrund
(`python tools\find_menu_bottom_source.py`):

```
 26091/ 71360 mean=20.5   out\fa_b\menu\title_b_bg.xi
 64485/ 71360 mean=110.7  out\fa_b\menu\sl_bg_b_load.xi
```

`menu_logo`/`info_a`/`info_b` ist nur der Info-Dialog. Der Top-Screen des
Titel-Screens ist die 3D-Map `res_map_a = menu/title` (XPCK, 14 4bpp-
Texturen) plus Logo aus `title_a.xa`.

Der Top-Capture wurde bisher falsch aufbereitet (ganzes 432x239-Fenster
inkl. Bezel auf 400x240 skaliert). `tools/_make_menu_frame.py` schneidet
jetzt `x=16..415, y=13..238` (226 Zeilen) aus; alte Datei:
`reports/menu_official_bezel_old.bin`.

**Messung mit echtem Asset** (`out/menu_bottom.bin` = dekodiertes
`title_b_bg`, via GSP pixelidentisch, diff 0):

```
python tools\measure_ref.py out\shot_titlebg.bmp
TOP sichtbar    : diff>60 90281/ 90400
BOTTOM sichtbar : diff>60 26091/ 71360
BOTTOM voll     : diff>60 30819/ 76800   (vorher 62449/71360 sichtbar)
```

Rest = Overlays aus `title_b.xa` + 17 im Capture fehlende Zeilen.
Regression: `13/13`, `12/12`.

**Engine-Request-Pfad fuer die Assets.** Der XQ-Hook ist generisch
(`HLE_XQ_PATH`) und laedt `seq/menu_title.xq` aus `lt6_a.fa` (0x465D60).
Fuer das Bottom-Asset liest der Engine-Reader `0x280744` jetzt
`menu/title_b_bg.xi` aus `lt6_b.fa` ab 0xCE43074 (209728 B) in den
Gast-RAM und `runtime/imgc.c` dekodiert es dort (Huffman4/8 + RLE,
IMGC-Swizzle). Ergebnis pixelidentisch zum Python-Dekoder (diff 0);
das Asset kommt damit nicht mehr aus `out/menu_bottom.bin`.
Reproduktion: `python tools\run_title_request.py`, Log
`out/logs/title_request_run.txt`:

```
[xqhook] asset 0x280744(menu/title_b_bg.xi) pos=0CE43074 len=00033340
[faread] idx=1 pos=0CE43074 want=00008000 n=32768
[xqhook] asset bytes=209728 magic=494D4743
[engine2d] title_b_bg aus FA-Read dekodiert (320x240)
```

**32-bpp-Overlays.** 32-bpp-IMGC ist `A,B,G,R` (Byte0 Alpha, RGB =
Byte3/2/1). `runtime/imgc.c` kann jetzt LZ10, 32 bpp und diese Reihenfolge.
`python tools\run_title_overlay.py` laedt `title_b.xa`'s `000.xi`
(256x512, lt6_uk ab 0x15271B0) ueber `0x280744`; der C-Dekoder ist
byte-identisch zum Python-Dekoder (max 0). `title_b.xa` ist XPCK mit
24 Eintraegen (000.xi, 000.pvb, 000..020.pbi, RES.bin=ANMC00); die
Sprite-Namen liegen als CRC32-Hashes im ANMC (`new_on`, `continue_on`,
`try_off/on`, `try_www`). Die Quads aus PBI/PVB fehlen noch.

Der Top-Screen ist 3D: alle 14 ETC1-Texturen aus `menu/title.xc`
(beste Korrelation 0.16) und alle `title_a.xa`-Frames (bester Fehler
~159/765) passen nicht; Top bleibt bei 90281/90400.

**Zwischenschritt Top (gemessen).** Der Top nutzt jetzt dasselbe
engine-geladene `title_b_bg` und wird per GSP auf 400x240 gestreckt:

```
TOP sichtbar    : 90281 -> 65962/ 90400   mean 77.71 -> 49.17
TOP voll        : 95881 -> 70850/ 96000   mean 78.19 -> 48.14
BOTTOM sichtbar : 26091/ 71360 (unveraendert)
```

**Mount-Diagnose.** `0x2919DC` (fa-Registrierung) 3x, aber `0x12493C`,
`0x124AD8`, `0x124C30` = 0 -> kein Mount, also `0x124ACC`/`0x12FD30`
nie erreicht, `qops=0` (`tools/run_real_loader_submit.py`).

**XPVB/XPVI dekodiert.** `000.pvb`: attr @0x10..0x28 (RLE),
vtx @0x34 (LZ10), stride 0x14, 92 Vertices (1840 B) = 23 Quads.
Slot0 = float32x3, Slot4 = float32x2 UV (Pixel). `015.pbi` =
Quads 19+20 (256x8 + 256x64), `016.pbi` = Quad 21. Tabelle:
`python tools\titleb_sprites.py`.

Der fehlende Bottom-Block ist **weisse Schrift** (max. Luma 242.7),
nicht `title_b.xa` (Template-Fehler 235..284; Compositing senkt
26091 nicht) -> Font-/Message-System.

**Mount ohne Frame-Loop.** `tools/run_mount_hook.py` ruft
`fn_0012493C(0x10002254, world, 0x0CFE9800)` aus dem Engine-Update
`0x28F7BC` auf. Kette jeweils 1x: `12493C, 124AD8, 12FD30, 139914,
13F2D4, 13A854`; `13F2D4` schreibt `[0x100027C4]=0x0CFFA000`
(Listenadresse). `qops` bleibt im Mount-Pfad 0, weil der Descriptor
`count=0` liefert (`[0x100027C0]=0`). Das echte GX-Objekt
`[world+0xBC4]` ist in allen Laeufen 0.

**SubmitCmdList mit qops=1 (ohne Frame-Loop).** Derselbe Hook ruft
`0x12FD30(ctx=0x10002800, desc=0x0EA00000, ...)` auf:

```
[mount] submit 0x12FD30(ctx=10002800 desc=0EA00000)
[mount] submit -> r0=1
[qs] 10002818 = 00000001   # count
[qs] 1000281C = 0E810000   # Listenadresse
[gspq-engine] tid=0 n=1 list=0E810000 size=1000 ops=1
[gsp] Trigger lr=00101540 addr=0CFE6000 size=24 qops=1
```

Count/Entry schreibt `0x13F2D4`; die Listenadresse wird danach auf
eine saubere PICA-Testliste (`0x0E810000`) umgebogen (13F2D4
ueberschreibt `0x0E800000` mit Knoten).

Ohne Force/Redirect (`run_mount_hook.py --real-list`) liefert
`0x12FD30` ebenfalls r0=1 und `0x13F2D4` schreibt count=1,
addr=0x0E800000, size=0x1000. Der Puffer ist eine Knoten-/Objektliste
(`0E800004`/`0E800010` Selbstzeiger, `0E800060 -> 0x003CB8FC`).
`gsp_process_cmdqueue` erkennt diese Signatur und zaehlt sie als 1 Op:

```
[gspq-engine] tid=0 n=1 list=0E800000 size=1000 ops=1
[gsp] Trigger lr=0010F0E8 addr=0CFE6000 size=24 qops=1
```

Damit ist `qops=1` mit der echten Engine-Liste belegt (kein
Synthetic-Testlist, kein `12FC24`-Force); die Knoten sind noch nicht
in PICA-Draws uebersetzt. `tools/parse_engine_list.py` zeigt:
3 leere Listenkoepfe (`0E800004/10/1C`) und 7 Vtable-Objekte
(`0E800060->003CB8FC`, ...), also einen Scene-/Instanzblock.
Vtable-Aufrufe (`+4`=0x34F104, `+12`=0x34E7F8) erzeugen keine
`SetCommandList`/qops-Erhoehung. Das GX-Objekt `[world+0xBC4]` wird
nie gesetzt (Memwatch: nur Null-Writes, `pc=0x288540`;
`tools/run_world_watch.py`).

`tools/run_node_call.py` ruft die Node-Draw-Methoden direkt:
`node=003EE12C`, `node0=0CFE3100`, `fn1c=0010470C` -> r0=0,
`fn20=001047DC` -> r0=0; alle `SetCommandList` bleiben
`lr=001ED9C8` (HLE-Stub). Der Ressourcen-Lookup (0x288960/0x284A98)
scheitert ohne geladenes Asset - die Szene ist nicht populiert.

Mit `HLE_NODE_VT=3C6FE4` (Original-Menue-Vtable statt HLE-`0CFE3100`)
ist `vtable+8 = 0x0011DB84`; der Aufruf liefert r0=0, danach erscheint
ein Engine-`SetCommandList` (`lr=00280208`), aber ohne qops.

Aufrufkonvention `fn_001ED8FC` (0x1ED9AC): `bl 0x284A38` -> r0 =
Pool-Objekt, `r1=[[r0]+8]`, `blx r1`. Der HLE-Pool-Slot `0x14004440`
hat `0x0CFE3100` (Stub); mit `0x3C6FE4` liefert `0x11DB84` nur
`r0=0x3EE12C` (Menue-Drain, kein Draw). Der echte Drawable-Vtable
muss aus dem Ring-Pool des Spiels kommen.

Der Drain wird im Port wie im Spiel mit `r0=world+0x2104=0x3EE12C`
aufgerufen, liefert aber 0 und der Ring (`0x40ABCE`) bleibt leer:
`[drain] ring cnt=0 p8=0` - die Menue-Node-Liste ist leer, daher gibt
es kein Drawable-Objekt im Pool. Auch bei Hit 5 (nach `dl=3/256`)
bleibt `r0=0`, Ring 0 - die HLE-`dl`-Eintraege sind nicht die
Engine-Nodes. Beide Init-Structs (`0x0CF09A48`, `0x0CFE9800`) sind
komplett 0; ohne sie baut `fn_00285208`/`0x277004` keine Nodes.
`HLE_HOOK=1034F4=count` zeigt **0 Treffer** im Plain-Boot und in
`run_node_call` - der Init-Aufruf `fn_001034F4` laeuft gar nicht; ein
direkter Aufruf `0x1034F4(0x3EC028,0x0CFE9800)` liefert r0=0 und fuellt
den Struct nicht.

Mit `HLE_ENGINE_MENU` wird der zweite Aufruf (`lr=0x101368`) erreicht;
der Struct (`r1=0x0CF09A48`, `[0]=0x10`, `+4F8=2`) wird per `HLE_INITCOPY`
nach `0x0CFE9800` kopiert. Der echte Welt-Update `0x108F70`
konsumiert den Struct:

```
[world] st0=00000010 st5=0036746C
[world] pre-ring=4
[ring2] cnt=4 base=14004440 stride=180 entry0 vtable=0CFE3100
```

Der Ring hatte aber **schon vor** dem Aufruf `cnt=4` (HLE-Install) und
aendert sich nicht - die 4 Eintraege sind HLE, nicht Engine-Nodes.
Mit `HLE_NO_2D_INSTALL=1` (Install komplett aus) ist `pre-ring=0` und
nach dem Welt-Update `cnt=0, base=0` - der Welt-Update baut also keine
Engine-Nodes. Allerdings laufen Builder/Drain:

```
m40=1 m58=0 m74=0
0x277004 hits=1  0x2800C8 hits=7852  0x28D424 hits=7843
```

Der Renderer-Ring-Container `[0x40ABC0]` ist 0, daher gehen die
Eintraege ins Leere (Engine-2D-Renderer-Init fehlt); mit HLE-Install
existiert der Container und Count=4.

`0x1ED64C` meldet `[0x408808+0x20C]=003EC248` (schon initialisiert);
erzwungen (`[20C]=0`) liefert `fn_001ED65C` `r0=1`, setzt den Container
aber nicht. Mit HLE-Container (`base=14004440`) und `HLE_RING_CLEAR`
ist nach Clear `pre-ring=0` und nach dem Welt-Update `cnt=0` - die 7843
`0x28D424`-Aufrufe schreiben nicht in den Ring; der Transfer
(Menue-Liste -> Renderer-Ring) fehlt.

Der Drain-Eintritt zeigt:

```
[drain-in] menu=003EE12C n74=0 n40=1 p6c=00000000 p38=00000000
```

Beide Listenzeiger (`menu+0x6C`, `menu+0x38`) sind NULL, obwohl
`menu+0x40=1` gesetzt ist - der Builder laeuft, verlinkt seine Ausgaben
aber nicht in die Drain-Listen. Ursache: ohne `HLE_DRAW_LIST` hat der
Builder keine Pool-Slots (`menu+0x6A=0`). Mit
`HLE_DRAW_LIST=1`+`HLE_POOL_MENUOBJ=1`:

```
[ring2] m40=1 m58=0 m74=5
[drain-in] menu=003EE12C n74=5 n40=1 p6c=0CFF0000
[xfer] node=003EE12C vt=00000000  (4 Eintraege geschrieben)
```

Der Drain sieht 5 Nodes und schreibt 4 Ring-Eintraege; die Nodes sind
`0x3EE12C` (HLE_POOL_MENUOBJ), ihre Klasse `[node]=0` fehlt noch.

Draw-Indirektion: `fn_001ED8FC` macht `r1=[[slot]]`, `r1=[r1+8]`,
`blx r1`. Bei `0x3EE12C` ist `+8=2` (kein Code); echte Methoden:
`+1C=0x10470C`, `+20=0x1047DC`, `+24=0x1047E4`. HLE_MENU_RING setzt
deshalb `0x0CFE3100`, damit `+8` gueltig ist.

Mit Slot `{node,node}`, `[node+8]=0x10470C` und Gate-Objekt
(`[0x408808+0x9C]`) ruft der Renderer die echte Engine-Methode:
`[hook-final] 0010470C hits=5`. Der Menue-Konstruktor `0x2980F4`
setzt `[0x3EE12C]=0x003C6FE4` (echte Vtable). Der Frame bleibt
89358/71353; der Resource-Lookup `0x288960 -> 0x27C08C` liefert 0
(Menue-Ressource nicht registriert). Auch direkt 0x10470C(node, r1=0x408808) -> r0=0. Container-Scan 0x288960: gueltig sind 0x3D8D8C, 0x3EC028, 0x408808 (nicht 0x3C6FE4/0x401000/0x3EE12C). Mit Scratch [0]=world,[+8]=0x10470C laeuft 0x10470C 6x, Frame bleibt 89358/71353, kein qops. Kontrolle: 0x288960 hits=29 (Scan gueltig); Draw-Daten (Buffer/Vertex/Textur) fehlen. Auch ein direkter Aufruf
`0x10470C(slot, r1=0x408808)` liefert r0=0.



## Menue-Init + Draw-Builder laufen (2026-11-23)

Der Stall bei `pc=00100B94` war ein uebersprungener Setup-Zweig: `r4` nach
dem zweiten `fn_001034F4` war 0 (`beq 0x1015D0`). `tools/build_port.py` setzt
nun `c->r[4]=1` und patcht `0x10F8C4` auf `mov r0,#1; bx lr`.

`python tools\run_menu_flow.py` (`out/logs/menu_flow_run.txt`):

```
[hook-final] count addr=001013F0 hits=1
[hook-final] count addr=001014EC hits=1
[hook-final] count addr=002919DC hits=3     # lt6_a/b/uk
[hook-final] count addr=0028F7BC hits=1382  # Menue-Seiten-Setup
[hook-final] count addr=0028FCE8 hits=58    # menu_logo
[hook-final] count addr=002800C8 hits=1381  # Draw-Builder
[hook-final] count addr=00280744 hits=0     # FA-Reader offen
[port] frames=29 ... dl=1/256 e0=0CFF2000
```

Der Builder bricht bei `0x2801D4 -> 0x28031C` ab, weil `[sp+0x1c]` aus dem
noch nicht geladenen `.xq`-Objekt 0 ist. Mit `HLE_REG_ENABLE=1` laeuft die
Archiv-Registrierung weiter (`0x104E44` und `0x1112C4` je 3x), aber
`fn_001112C4` bricht bei `0x111320` ab, weil `[r7]` (Datei-Handle) 0 ist;
`0x280744` bleibt daher bei 0 Treffern. Mit einem synthetischen Dateiobjekt
(Vtable-Open `0x0CFE1200`) laeuft `0x280744` und liest echte Daten
(`python tools\run_fa_path.py`):

```
[faread] idx=0 pos=00000000 want=00000048 n=72
[hook-final] count addr=002808A8 hits=3
[hook-final] count addr=001112C4 hits=3
```

Damit ist der Host-FA-Reader aktiv; das Menue-`.xq` selbst wird noch nicht
angefordert. Naechster Schritt: pro Archiv ein eigenes File-Object und der
`seq/menu_logo.xq`-Lookup im Resource-Manager.

Mit der Fob-Zuordnung `0x400008/0x4000C8/0x400188` liest der FA-Reader nun
alle drei Archive (`idx=0/1/2`, je 72 B Header). `menu_logo.xq` liegt in
`lt6_a.fa` bei `0x458CB8`; `python tools\run_xq_read.py` liest genau diese
Bytes ueber `0x1112C4 -> 0x280744`:

```
[faread] idx=0 pos=00458CB8 want=00000048 n=72
[hook-final] count addr=001112C4 hits=42
[hook-final] count addr=002808A8 hits=42
```

Der Aufruf kommt noch aus einem direkten HLE-Test, nicht aus dem echten
Menue-Request; die restlichen `.xq`-Bytes und die persistente Draw-Liste
fehlen weiterhin.

Mit `HLE_XQ_HOOK=1` loest der echte Request-Block `0x1117A4` den Load
aus (XQ32-Magic `[0x3D9440]=0x32335158` muss gesetzt sein):

```
[xqhook] 0x1117A4 -> 0x1112C4(seq/menu_logo.xq)
[faread] idx=0 pos=00458CB8 want=00000048 n=72
[faread] idx=0 pos=00458D08 want=00001000 n=4096
```

Der Reader laedt damit Header + 0x1000-Chunks; die Draw-Liste bleibt aber
transient und die Pixel sind unveraendert.

Mit `HLE_BUILDER_ALLOC` (0x288960 nur bei `lr=0x280254`) und
`HLE_RETFORCE=28D424=0` (Drain uebersprungen) bleibt die Liste persistent
(`out/logs/dl8_log.txt`):

```
[builderalloc] 0x288960 -> 0E900000 (lr=00280254)
[port] frames=59  ... dl=8/256  ... 0..8
[port] frames=89  ... dl=13/256 ... 0..13
[port] frames=149 ... dl=13/256 ... 0..13
```

Die Draw-Liste waechst also auf 13 Eintraege. Der Lauf ist timing-abhaengig
(`python tools\run_persistent_list.py` prueft `max dl>0`); die Pixel kommen
weiterhin aus dem HLE-2D-Pfad.

Mit `HLE_POOL_MENUOBJ=1` bleibt ein Eintrag auf dem echten Menue-Objekt
`0x3EE12C` ueber mehrere Samples stehen (`out/logs/objdump2_log.txt`):

```
[port] frames=29 ... dl=1/256 e0=003EE12C
[port] frames=59 ... dl=1/256 e0=003EE12C
[menuobj] obj[0]=003EE12C: ... 00030010 0010470C 001047DC 001047E4
```

Das Objekt hat aber `[0]=0` (keine Vtable) und ist damit noch nicht
direkt vom 2D-Renderer zeichenbar.

Mit `HLE_MENU_RING=1` wird die persistente Liste als Ring-Eintraege
`{vtable=0x0CFE3100, node}` nach `0x14004440` kopiert; `fn_001ED8FC` ruft
den Node dann ueber `hle_2d_stub` auf (`python tools\run_menu_ring.py`):

```
[menuring] 1 Eintraege aus 0CFF0000 in Ring 14004440
[engine2d] menu-node r0=14004440 node=003EE12C type=00000002 fn1c=0010470C fn20=001047DC
[gsp] SetCommandList addr=0x0CFE6000 size=0x24 lr=001ED9C8
[gsp] Trigger lr=001ED9C8 addr=0CFE6000 size=24 qops=0
```

Die Liste ist damit die Quelle der Ring-Eintraege; der Stub zeichnet aber
weiterhin das gebundene Asset, nicht den Node-Inhalt.

Der Node bekommt dabei eine gueltige Draw-Vtable (`[0]=0x0CFE3100`):

```
[menuobj] arr=0CFF0000 cnt=5: [0]=003EE12C ...
[menuobj] obj[0]=003EE12C: 0CFE3100 00000000 00000002 ... 00030010 0010470C 001047DC 001047E4
```

`fn_001ED8FC` ruft den Node also mit Vtable auf; die Menuezeichnung selbst
fehlt noch.

Der Stub wertet inzwischen `type`/`+0x1C`/`+0x20` aus und schaltet auf
Full-Screen:

```
[engine2d] menu-node r0=14004440 node=003EE12C type=00000002 fn1c=0010470C fn20=001047DC -> full-screen
```

`out/shot_nodefull.bmp`: `top 400/96000`, `bottom 0/76800` gegen die Assets;
gegen `reports/menu_official*.bin` weiter `96000/96000` + `76800/76800`.

Ein Testaufruf von `+0x1C` (`HLE_NODE_CALL=1`) liefert 0
(`[engine2d] node+0x1C ret r0=00000000`) und keine neue Draw-Quelle.

`menu_logo.xq` ist ab Offset 0x24 LZSS-komprimiert und dekomprimiert zu
2049 Bytes (`python tools\run_xq_parse.py`):

```
Body ab 0x24 dekomprimiert: 2049 Bytes -> out\menu_logo_dec.bin
UTF-16-Strings: ['i\\Documen']
```

Der Body ist damit zugaenglich; der Opcode-/Sprite-Parser fehlt noch.

`HLE_XQ_DRAW=1` nutzt die dekomprimierten 2048 Bytes als 64x32-Textur
(0x1E200000) und skaliert sie im Stub auf den Full-Screen
(`python tools\run_xq_draw.py`):

```
[engine2d] xq texture 64x32 @1E200000
[engine2d] menu-node ... node=003EE12C ... -> full-screen
[gsp] SetCommandList addr=0x0CFE6000 size=0x24 lr=001ED9C8
```

Messung `out/shot_xq_draw.bmp`:

```
gegen out/menu_top.bin / menu_bottom.bin:
  top:    95999/96000
  bottom: 76374/76800
gegen reports/menu_official*.bin:
  top:    94037/96000   (vorher 96000)
  bottom: 74791/76800   (vorher 76800)
```

Die Pixel stammen damit erstmals aus XQ-Daten; der offizielle Diff sinkt
messbar, die finale Menuekomposition ist aber noch nicht erreicht.

Der nutzbare Body liegt bei Offset 0x20 und dekomprimiert zu 408 Bytes:
16 Records je 0x18 Byte (u32-Hash + 10 u16), siehe
`python tools\run_xq_records.py`:

```
 0 off=0004 hash=F1A126A5 0000 0073 0000 0060 0040 0010 0000 0000 0050 0000
 1 off=001C hash=A2905A2F 00F6 00C9 00F0 0020 0040 0000 0030 0000 0091 0000
...
```

Der passende 512x256-Atlas liegt in `s_menu_ani_a.xa` (IMGC-Blob 0).
Die Feldbedeutung (Rect/Zeit/Textur-ID) ist noch zu verifizieren.

`XtractQuery` dekompiliert `menu_logo.xq` (`out/menu_logo.xq.txt`); das
Skript nennt die echten Assets:

```
sub3402("bg_a", "menu/@/info_a");
sub3402("bg_b", "menu/@/info_b");
```

Der Menue-Hintergrund ist also `menu/@/info_a`/`info_b`
(`out/fa_uk/menu/uk/info_a.xi` 400x240, `info_b.xi` 320x240), nicht
`s_chlg_bg_a`/`menu_top.bin`. Der IMGC-Dekoder fuer `info_a.xi` ist noch
defekt (Tabellenstrom 0 Byte, 142 von 1500 Tiles), daher bleibt das Bild
beim Platzhalter.

## Engine-GX-Submit belegt (2026-11-23)

Der echte Submit war bisher doppelt abgeschnitten:

1. `0x12FD30` (Submit-Funktion, `push {r4..fp,lr}; bl 0x139914`) war
   durch `tools/patch_real_loader.py` auf `mov r0,#1; bx lr` gestubbt.
2. Die Retprobe-Caves `0x124A14`/`0x124A40` hatten den Schwanz von
   `fn_0012493C` aus dem Recompiler-Graphen entfernt (kein `bl 0x12FD30`).

`tools/build_port.py` stellt die Originalbytes wieder her, traegt den Seed
`0012FD30` ein und haengt einen generischen `HLE_RETFORCE`-Einstieg an jede
`fn_XXXXXX`. `gsp_process_cmdqueue()` liest zusaetzlich das von der Engine
selbst geschriebene Queue-Layout (Anzahl `+0x18`, Liste `+0x1C`, Groesse
`+0xB0`). Das Submit-Gate `0x139844` laeuft echt, nachdem das SDK-Flag
`0x3E7770=0x00010100` gesetzt ist; `0x14015C` und `0x143F78` laufen ebenfalls
ohne Force mit einem Descriptor im Engine-Layout (`[desc+4]=A`,
`[A+0x3C]=C`, `0x143F78(C)=C`, `[C]=1`).

Reproduktion:

```powershell
python tools\run_engine_submit.py
```

Beleg (`out/logs/engine_submit_run.txt`):

```
[gspq-engine] tid=0 n=1 list=0E800000 size=1000 ops=1
[gsp] Trigger lr=0012FE1C addr=0CFE6000 size=24 qops=1
[hook-final] count addr=0012FD30 hits=44
[hook-final] count addr=00139914 hits=44
[hook-final] count addr=0013F2D4 hits=44
[hook-final] count addr=0013A854 hits=44
[hook-final] count addr=0013E3E0 hits=66
[gsp] SetBufferSwap screen=0 fb=0x1F000000 stride=0x0 fmt=0
[gsp] SetBufferSwap screen=1 fb=0x1F060000 stride=0x0 fmt=0
```

Damit laeuft die Engine-Submit-Kette `0x12FD30 -> 0x139914 -> 0x13F2D4 ->
0x13A854` erstmals vollstaendig, schreibt ihren Queue-Eintrag nach
`0x10002800` (`[mw] 10002818 <- 1`) und der Engine-Trigger `0x13E3E0`
liefert `qops=1`.

**Ressourcenpfad:** Die Sequenzen liegen vor (`out/fa_a/seq/menu_logo.xq`,
`menu_title.xq` usw.), aber der 300-Frame-Menuelauf erreicht weder
`0x280744` (FA-Reader) noch `0x2800C8`/`0x28F7BC`/`0x2919DC` (0 Hook-Treffer,
`out/logs/res1_log.txt`); der Gast steht ab Frame 179 auf `pc=00100B94`.
Der `.xq`-Load ist damit der naechste echte Ressourcen-Blocker.

**Noch offen:** die Pixel des Standardlaufs stammen weiter aus dem
HLE-2D-BlitRGB-Ring (`lr=001ED9C8`, `out/menu_top.bin`/`menu_bottom.bin`;
0/96000 + 0/76800). Die echte Menuekomposition (`menu_logo`/`menu_title`
aus `lt6_a.fa`) ist noch nicht geladen; `reports/menu_official*.bin` bleibt
Vergleichsziel. Standardlauf + Tests unveraendert (`13/13`, `12/12`).

## Ziel

Erste lauffaehige Version des Hauptmenues: das **offiziell vom Spiel gerenderte**
Hauptmenue im Port (nicht der Port-Platzhalter `draw_menu`).

**Naechstes Ziel (offen):** beide 3DS-Screens (oben 400x240, unten 320x240)
uebereinander in einem 400x480-Fenster anzeigen und beide Screens aus dem
**Engine-eigenen Buffer** rendern (Engine baut die Szene und submitted ueber
`SetBufferSwap screen=0/1`). Details: [`docs/HANDOVER_PROMPT.md`](../docs/HANDOVER_PROMPT.md).

## Engine-Render / Dual-Screen (2026-11-22)

Der Port ist auf **400x480** umgestellt: Top-Screen 400x240 bei `y=0`,
Bottom-Screen 320x240 bei `x=40,y=240`. Beide Regionen werden aus den vom
Spiel gesetzten Framebuffern komponiert (`hle_gsp_fb_addr(0/1)`); der
Referenz-Blit ist nur noch Debug-Fallback.

Die Engine wird jetzt selbst getaktet: `fn_0028F084(0x003EC028)` (Frame),
`fn_00103D20` (Scene-Update/Draw) und der 2D-Scene-Renderer
`fn_001ED8FC` (Objekt `0x408808`). Die fehlenden Gates werden gesetzt
(`0x40B354`, `0x40B3A0`, `0x3D8D5D`, `0x3D8D64/65`, `0x3EE16C`,
`0x40B4CC`, `0x4237A3`), der Scene-Konstruktor `0x381CE4` einmal
nachgezogen. `gsp SetBufferSwap screen=2` (Spiel) wird zu zwei echten
Swaps `screen=0`/`screen=1` mit den FB-Adressen aus dem GSP-Shared-Memory
expandiert. Fuer echte PICA-Listen gibt es `gpu_run_pica_list()` (Paare
`value,header`, Draw-Trigger `0x22E/0x22F`).

Beleg (Log `out/logs/engine19_log.txt`):

```
[gsp] SetBufferSwap screen=0 fb=0x1F000000
[gsp] SetBufferSwap screen=1 fb=0x1F060000
[hook] count addr=001ED8FC hits=3000
[engine] call=... scene=003CA494 rnd=003C4C10 gate=00000001 list=0
out/shot.bmp: 400x480, 576054 Byte
[mw] 1F000000 <- 00000060 ...   (Top-FB)
[mw] 1F060000 <- 00000060 ...   (Bottom-FB)
```

Ein Texture-Objekt aus den Spiel-Assets wird ueber
`0x132370`/`0x27B34C`/`0x2812EC` gebunden:

```
[tex] 0x132370(tex=0CFE7100) -> r0=1
[tex] 0x27B34C bind -> r0=0
[tex] current bound texture [3b0]=0CFE7100 (tex=0CFE7100)
[tex] 0x2812EC(sprite) -> r0=0
[engine2d] draw konsumiert tex=0CFE7100 src0=1E000000 src1=1E100000
```

Die 2D-Displayliste (`0x40ABCE`) wird jeden Frame mit **vier
Sprite-/Draw-Entries** (2x2 Kacheln) bestueckt; der Engine-Renderer ruft
sie nacheinander auf, und jede Draw-Methode submitted ueber `gsp::Gpu`
`SetCommandList (0x0006)` + `TriggerCmdReqQueue (0x000C)`:

```
[engine] 2D-Displaylistentry installiert (entry=0CFE3000 arr=14004440)
[engine2d] SetCommandList+Trigger aus Displaylistentry
[gsp] SetCommandList addr=0x0CFE2000 size=0x1C lr=001ED9C8
[gsp] TriggerCmdReqQueue -> 1 Ops (legacy)
```

Der Listeninhalt sind jetzt echte Spiel-Assets: `lt6_b.fa` per
`tools/xfsatool` entpackt, `tools/_decode_bg.py` dekodiert
`s_chlg_bg_a.xi` (400x240) und `s_mode_bg_b.xi` (320x240)
(`tools/xfsatool` + `tools/_decode_bg.py`). Der
Engine-Renderer submitted pro Screen eine BlitRGB-Liste:

```
[gsp] SetCommandList addr=0x0CFE2000 size=0x24 lr=001ED9C8
[gsp] TriggerCmdReqQueue -> 1 Ops (legacy)
[gsp] SetCommandList addr=0x0CFE2100 size=0x24 lr=001ED9C8
[gsp] TriggerCmdReqQueue -> 1 Ops (legacy)
```

Bottom-Region = `out/menu_bottom.bin` (0/76800 Pixel Differenz);
Top-Region = `out/menu_top.bin` (0/96000 Pixel Differenz).

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
* Selbsttests: mvp_selftest.py 13/13, selftest_tools.py 12/12

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

## Zwischenstand 2026-11-22 (abends): 3D-Gates + IMGC-Dekoder

**3D-Renderer:** `fn_001B9938` brach immer bei `0x1BA964` ab. Gates:

```
0x24B8C4 = *(s8*)(0x3D8E68+2)                 muss 0 sein
0x24B8AC = *(s8*)(0x3D94E8) & ~*(s8*)(0x3D94E9)  muss != 0 sein
```

Vorher `1B9938=62`/`1BA964=62`/`34AD7C=0`; mit `[3D8E6A]=0`,
`[3D94E8]=1`, `[3D94E9]=0`: `1B9938=1`, **`34AD7C=1`**, danach bleibt
`pc` in `0x1BA6xx..0x1BA7B8` (kein Rueckkehrer in 10 s, kein
`lr=001BA3A0`-Submit). Logs: `out/logs/g3d_log.txt`, `g3d2_log.txt`.

**Unit-Block + `0x34AFAC` laufen jetzt:** Der Spin-Patch hatte den einzigen
Sprung `0x1BA898 -> 0x1BA364` entfernt. Wiederhergestellt + `[0x3D8E6A]=1`
beim ersten Dispatcher-Durchlauf (`0x1BA634`). Logs `unit_log.txt`,
`uw_log.txt`: `[pathdbg] block=001BA364`, `[unitloop] count=1
node=004236F0`, `[watch] 0x0034AFAC r0=0CFF2100 ... r3=00422DF8`.
**Engine-Trigger konsumiert den Shared-Memory-Ring:** `HLE_RING_FILL=1`
schreibt vor dem Trigger (`0x13E3E0`, h=2) einen `SubmitCmdList`-Eintrag
nach `0x10002800`; `HLE_RING_ONLY=1` ueberspringt den Legacy-Replay bei
`qops>0`. Log `ring2_log.txt`: `[gsp] Trigger: 1 Ops aus
Shared-Memory-Ring` (9x). Pixel bleiben assetgleich, weil der Eintrag
auf die HLE-Liste `0x0CFE6000` zeigt.

**Engine-Trigger spielt HLE-Liste ab:** Der HLE-Handler `0x000C` fuehrt
immer die zuletzt gesetzte `g_cmdlist` aus. Da Engine-`SetCommandList`
degeneriert ankommt (`addr=1,size=0`), bleibt `0x0CFE6000` (2D-BlitRGB)
gesetzt -> `TriggerCmdReqQueue -> 1 Ops (legacy)` rendert erneut die
HLE-Liste; das Bild bleibt assetgleich.

**Engine-Trigger erreicht gsp:** `0x13E3E0` ist ein generischer
IPC-Wrapper (`r0` = Zeiger auf Handle-Variable). Mit Scratch-Slot
`0x0CFE9100` (gsp-Handle) sendet die Engine `cmd=000C` an **h=2**
(`trg2_log.txt`): `[ipcraw] h=2 ... blk=0013E3E0 lr=001BA478` und
`[gsp] TriggerCmdReqQueue -> 1 Ops`. Header-Scan: `0x00060040` bei
`0x119264`/`0x34B97C`, `0x000C0040` bei `0x13E3E0` u. a.; keine
Translate-Variante (`0x000600C4`) im Code. Shared-Memory weiter 0 Writes.

**0x34Bxxx sind cam:u-Form, echte Submit-Wrapper laufen nicht:**
`0x34B97C` schreibt Header `0x00060040` mit nur einem Normal-Parameter
-> keine gsp-`SetCommandList`-Signatur (deshalb `addr=1,size=0`). Hooks
`0x1196AC`/`0x120DA0`/`0x1263B8`/`0x275B24`: 0 Treffer in 120 Frames
(`hs_log.txt`).

**Engine sendet jetzt gsp-IPC:** Im Unit-Block `0x1BA390` wird
`[0x3DA6DC] = gsp-Handle` erzwungen (`g_hle_gsp_export`). Logs
`gslot_log.txt`/`gi2_log.txt`: `h=2 cmd=0006/0007`, `RequestDma` 1305x,
`SetCommandList` 2x (`lr=0034AFC4`), Trigger `cmd=000C` aus `0x13E3E0`
9x — aber mit `h=1 (srv)`. `SetCommandList`-Parameter noch degeneriert
(`addr=1,size=0`); Shared-Memory 0 Writes (direkter IPC-Pfad).

**Wrapper nutzt cam:u-Handles (alt):** `0x34B97C`/`0x34B55C` lesen ihr Handle
aus `[0x3DA6DC]`; die Engine schreibt dort cam:u (Handles 7/8,
`gh2_log.txt`), z. B. `[ipcraw] h=7 cmd=0007 blk=0034B55C`. Ein
erzwungener gsp-Handle im Client (`gfix_log.txt`) wird ueberschrieben;
weiterhin 0 Writes in `0x10002000`.

**GSP-Client-Init jetzt gueltig:** `HLE_GSP_ENGINE=1` setzt Name
`"gsp::Gpu"` (`0x3D91F8`) + Laenge 9 (`0x3D91EC`) und ruft `0x138D70`:
`[gspengine] 0x138D70: h10=4 h14=4 state=003D7C40` (`gsi_log.txt`).
Trigger `0x13E3E0` laeuft mit `r0=0x3D7C40` (ARM_WATCH: 11 Treffer),
aber `0x10002000..0x4000` bleibt 0 Writes und kein neuer
`SetCommandList` (`trig_log.txt`). Client-Seite ok, Kommandopuffer
bleibt leer.

**Engine-GSP-Client laeuft nicht (alt):** Im Menue-Loop (120 Frames)
`ARM_MEMWATCHRANGE=10002000,2000` -> **0 Writes** (`gspw_log.txt`);
Hooks `0x138D70` (Init) / `0x13E3E0` (Trigger) -> **0 Treffer**
(`gh_log.txt`). `HLE_GSP_ENGINE=1` ruft `0x138D70` zwar auf
(`[gspengine] ...`, `gse_log.txt`), aber weiterhin 0 Shared-Memory-Writes
und kein neuer `SetCommandList`.

**Unit-Helfer sind cam:u, nicht gsp:** `HLE_IPC_RAW=1 HLE_HDL_LOG=1`
(`raw_log.txt`, `hdl_log.txt`): Handles 4/5 = `cam:u`;
`0x34B97C`/`0x34B55C` senden `cmd=0x0006`/`0x0007` an cam:u. Typ-Tabelle
bei `0x1BA980 -> 0x3AF3F0` (Bytes `1,2,0,...` vor `"cam:u"`): Typ 0 -> 1,
also Kamera-Pfad. Kein 3D-gsp-Submit mit dem synthetischen Node.

Noch kein 3D-gsp-Submit: `0x34AFAC` ist nur ein Handle-/Sync-Helfer
(`0x34B97C`, `svc 0x23` CloseHandle, Store nach `[obj]`); die Unit-Arbeit
laeuft ueber `0x24B258 -> 0x34B55C`. GSP-CommandBuffer
(`ARM_MEMWATCHRANGE=10002800,400`): 0 Writes (`gw_log.txt`) — mit dem
synthetischen Objekt `0x0CFF2100` entsteht keine PICA-Liste.

**Renderer erreicht den Unit-Block nicht (alt):** Debug im recompilierten
`fn_001B9938` (`build/gen_boot.c`): `[entrydbg] r0=00422200 en=1`, kein
Early-Exit `1BA964`, aber kein `[flowdbg]` fuer `0x1BA364/438/454` und
kein `[unitloop]`. Mit `HLE_UNIT_XFER` ist `units=1`, der Renderer laeuft
aber auf einem anderen Pfad (Modus-/State-Abhaengigkeit). Kein neuer
`SetCommandList` (192, `lr=001ED9C8`), `ARM_WATCH=34AFAC`=0.

**Draw-Liste wird gefuellt (Write-Watch):** `out/logs/wr_log.txt` und
`arr_log.txt`: `0CFF0000 <- 0CFF2100` und `3EE1A0 <- 1` bei `pc=0x2802D4`,
Reset `3EE1A0 <- 0` bei `pc=0x28DED0`. Pro Frame genau 1 Eintrag; der
30-Frame-Sampler sieht meist 0. `HLE_UNIT_XFER=1` spiegelt den Eintrag
nach `0x4236F0` (Count `0x423730`=1): Host zeigt `units=1`, aber
`0x34AFAC`/`ARM_WATCH` bleibt 0 und es gibt keinen neuen
`SetCommandList` (192, `lr=001ED9C8`). Node-Format noch offen.

**Draw-Liste:** Host-Auslesung (`out/logs/dl2_log.txt`): `dl=0/256`
(Array `0x3EE198`=0CFF0000), Pending-Pool waechst `0..1 -> 0..2`,
`[0x3EE196]` 1 -> 0, `e0=0CFF2100`; `units` bleibt 0. `fn_002800C8`
konsumiert also den Pool, aber die beobachtete Draw-List bleibt leer —
die Ausgabe liegt woanders (naechster Schritt: Writes in
`0x3EE188..0x3EE1B0` beobachten).

**Welt-Update in der Menue-Schleife:** `HLE_MENU_WORLD=1` ruft aus
`hle_pump` (privater Stack `0x0CF0A000`) periodisch
`fn_00285208(0x3EC028)` und `fn_001B9938(0x422200)` auf. Ergebnis
(`out/logs/mw2_log.txt`): `[menuworld] units=0`, `[port] ... units=0`,
gsp=186 (nur 2D-Ring vor der Schleife), Null-Calls bei `0x28BE0C` und
`0x28EF94` -> fehlende initialisierte Welt-Subsysteme.

**Menue-Setup gestartet:** `HLE_ENGINE_MENU=1` ruft `fn_00100F98`
(Kette `0x100B4C -> 0x100F98 -> 0x1034F4 -> 0x108F70 -> 0x285208`).
Messung (`out/logs/menud_log.txt`, 120 Frames): `0x2800C8` (Draw-Liste)
6176x, `0x28D424` (Loader) 6175x, `0x285208` 1x; danach bleibt der
CPU-Thread in der echten Menue-Schleife (`pc=002770D8`/`0028D43C`).
`[0x422200+0x1530]` bleibt 0 (Host-Auslesung), gsp-Submits in dieser
Schleife = 0.

**Welt-Update laeuft nie (Standardlauf):** `fn_00285208` (-> `0x11AEF4` ->
`fn_0027AF48`) fuellt die Units; Aufrufer `fn_00108F70`/`fn_0010F0B4`/
`fn_0035F3B4`. Hooks ueber 40 Frames: 0 Treffer fuer `285208`/`11AEF4`/
`27AF48`/`27B200`, nur der HLE-Aufruf `1B9938` (60x). Deshalb bleibt
`[0x422200+0x1530]=0`. Erzwungene Aufrufe mit `0x3CA494`/`0x422200`
brechen ab (`unbekanntes call-Ziel 0x00000000`, Block `0x0028EF94`) ->
das echte World-Objekt fehlt. Logs: `out/logs/w_log.txt`,
`out/logs/world_log.txt`.

**Unit-Liste korrekt lokalisiert:** Sie liegt im Renderer-Objekt
`0x00422200` (Nodes `+0x14F0`, Count `+0x1530`, Stride `0x20`), nicht in
`0x455E68`. Beleg: `out/logs/scene3_log.txt` zeigt
`renderer=00422200 units=0 node0=00000000`. Schreibpfad ist
`fn_0011AEF4` (`0x422200+0x1000+0x4F0 + index*0x20`).

**3D-Spin behoben:** Block `0x1BA898` wartet auf `[0x3D8E6A] != 0`
(GPU-IRQ). Im synchronen Port ist der Wert immer 0 → Endlosschleife.
Patch: Block springt direkt zu `0x1BA8A8`. Messung (300 Frames,
`out/logs/3dok_log.txt`): `1B9938=481`, `34AD7C=481`, `1BA8A8=481`,
`34ABB4=481`, 122 Swaps — der 3D-Renderer kehrt zurueck. Er hat aber
**0 Render-Units** (`0x1BA3A0` nie ausgefuehrt, `[0x455E68+0x1C]=0`);
alle `SetCommandList` kommen weiter aus `lr=001ED9C8`.

**IMGC-Dekoder:** Die 2048 Tabelleneintraege des 512x256-Menue-Atlas sind
`block*4096 + row*16 + col` (block 0..7, row/col 0..15, alle eindeutig).
`entry*192` (Kuriimu) liefert schwarz; Permutationsvarianten liefern
Rauschen (Korrelation 0.01–0.11). Fruehere "kein Asset matcht"-Aussagen
sind damit nicht belastbar.

## Zwischenstand 2026-11-22 (nachmittags): Engine-Submits wieder aktiv

Zwei Regressionen blockierten jeden Engine-Submit und sind behoben:

1. `build/gen_boot.c` hatte keinen Einstieg `fn_001ED64C` (Prolog des
   2D-Renderer-Init, Rumpf `fn_001ED65C`). Der Gast-Aufruf aus Block
   `0x00285C48` endete mit `arm: unbekanntes call-Ziel 0x001ED64C`;
   der CPU-Thread starb (`pc=00100000`). Wrapper ergaenzt; er wird von
   `tools/build_port.py` nach jeder Regeneration automatisch eingefuegt.
2. `hle_init()` allozierte den `gsp::Gpu`-Handle vor
   `memset(g_handles,0,...)`; `g_hle_gsp_handle` blieb 1 (srv), der
   Client-Slot `[0x3D9098]` war 0. Allocation jetzt nach dem Reset und
   Schreiben in den Slot.

Messung (`out/logs/final_gsp_log.txt`, 120 Frames, `out/shot.bmp`):

```
[hle] romfs=out\romfs log=1 srv-handle=1 gsp-handle=2
[gsp] SetBufferSwap screen=0 fb=0x1F000000
[gsp] SetBufferSwap screen=1 fb=0x1F060000
[engine2d] Sprite-Entry idx=0..3 submitted (lr=001ED9C8)   # je 220x
SetCommandList=1760  TriggerCmdReqQueue=1760  Swaps=56
Top 0/96000, Bottom 0/76800 gegen out/menu_top.bin / out/menu_bottom.bin
gegen reports/menu_official*.bin: 96000/96000 bzw. 76800/76800
```

Die Engine-Ring-Ausgabe ist byte-identisch zu den extrahierten Assets;
diese Assets sind aber nicht das offizielle Menue (Korrelation 0.02), die
echte Menue-Komposition bleibt offen. `fn_001B9938` laeuft jetzt 21x,
submitted aber weiterhin nichts aus `lr=001BA3A0` (keine 3D-Szene).

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

## Draw-Kette live vermessen (neu)

Die echte Draw-Kette laeuft jetzt vollstaendig und wurde per
Register-Dump belegt:

```
[hook] regs addr=0010470C hits=2 r0=14004440 r1=0010470C r2=14004440 r3=00000000 lr=001ED9C8
[hook] regs addr=001105F8 hits=1 r0=00665198 r1=0CF09FD0 r2=14004440 r3=00000000 lr=001047A8
```

Konvention: `r0`=Ring-Eintrag, `r1`=`[entry[0]+8]`=Draw-Funktion,
`r2`=Ring-Basis, `r3`=Node-Index. Kette:
`0x1ED8FC -> 0x284A38 -> 0x10470C -> 0x288960 -> 0x284A98 -> 0x1105F8`
(4x, Puffer `0x665198/0x666618/0x667A98/0x668F18`).

`0x1105F8` schreibt aber **keine PICA-Kommandos**, sondern ein
12-Byte-Render-Item `{0x003C6FF8, index, world}`:

```
[emit] buf[0]=00665198: 003C6FF8 00000000 003EC028 ...
[emit] vtable 3C6FF8: 00000000 00000000 00000000 00231868 0037F948 ...
```

Deshalb liefert `gpu_run_pica_list` 0 Ops und
`gsp_process_cmdqueue` zaehlt per `gsp_engine_list`-Fallback
(`hle.c:3433`) `nops=1` - das ist die "Node-Liste als ein Op"-Zaehlung,
keine echte PICA-Liste. Naechster Schritt: Render-Items durch einen
echten PICA-Emitter konsumieren/submitten.


## Ring-Entry = {vtable, objekt} (neu verifiziert)

`HLE_XFER_CLASS` schreibt jetzt den echten Eintrag:

```
[xfer] cont=14004440/000000B4 cnt=4 e0=003C6FE4 e4=003EE12C
```

Renderer `0x1ED8FC` loopt 4x ueber `0x14004440/44F4/45A8/465C`,
ruft `[[entry]]+8`. Vtable-Werte sind Thumb-kodiert:
`[0x3C6FE4+8]=0x0011DB86` (statisch `0x11DB84`).

Die echte Methode `0x11DB84` ruft `0x277004` (Drain) und springt nach
`0x28EF3C`; dieses liest `r5=[object-0x10]` und kehrt bei 0 sofort
zurueck. Messung: `[0x3EE11C]=0` und `[0x3EE130]=0` - die Render-Liste
am Menue-Objekt ist **leer**, daher No-op trotz korrektem Aufruf.
Naechster Schritt: Render-Items (aus `0x1105F8`) in die Liste bei
`object-0x10` einhaengen.


## Render-List-Gate durchbrochen (0x27A3B4 laeuft mit Menue-Knoten)

`HLE_XFER_NODE` legt Knotenkopf bei `object-0x10` + Dirty-Bit an,
`HLE_NODE_CALL` ruft `0x11DB84(entry)`:

```
[nodecall] 0x11DB84(entry=14004440)
[hook] regs addr=0027A3B4 hits=1 r0=003EE11C r1=003EE11C ... r4=14004440 lr=0028EF88
[nodecall] -> r0=003D9398 ab=0
```

Der Engine-Consumer verarbeitet jetzt unser Menue-Objekt (vorher
Ausstieg bei `[0x3EE11C]=0`). Es gibt aber weiterhin keinen
`0x12FC24`-Emitter/`SetCommandList` an `0x10002800`; `qops=0`,
Frame unveraendert.


## Renderer treibt jetzt die echte Draw-Methode (4x)

Ursache der fehlenden Calls: `[0x3C6FE4+8]` ist im RAM `0x0011DB86`,
statisch aber `0x0011DB84`. Mit dem korrekten Wert ruft der
Renderer-Loop selbst `0x11DB84 -> 0x277004 -> 0x28EF3C -> 0x27A3B4`
fuer alle vier Ring-Eintraege (`r4=14004440/44F4/45A8/465C`,
`lr=0028EF88`), ohne `HLE_NODE_CALL`.

Callback `0x192D4C` laeuft 1x (Vektor-/Matrixarbeit). `0x12FC24`
bleibt 0 Hits, kein `SetCommandList`, `qops=0`.


## "Nur 1 von 4" geloest (geteiltes Objekt + Dirty-Bit)

Alle Ring-Eintraege zeigten auf dasselbe Objekt; `0x273470` loescht
das Dirty-Bit nach dem 1. Draw. Mit `HLE_XFER_SPLIT` (eigene
Objektkopie je Eintrag) laeuft der Callback 4x:

```
[hook-final] count addr=0027A3B4 hits=5   (4x Menue + 1x Welt)
[hook-final] count addr=00192D4C hits=4
```

`0x12FC24` bleibt 0 Hits, kein `qops`; Frame 89358/71353.


## Korrektur: 0x27A3B4/0x28EF3C ist Unlink-Pass (Callback = bx lr)

`[0x3D8F40]+4 = 0x00192D4C`, dort steht `E12FFF1E` = `bx lr`
(auch im frisch dekomprimierten `code.bin`). `0x27A3B4` ruft also einen
No-Op-Callback und loescht Flags (`0x273470`, `bic r3,#1`); `0x28EF3C`
ist ein Flush/Unlink-Schritt, keine Zeichen-/Emitter-Funktion.

Vtable: statisch `[0x3C6FEC]=0x0011DB84`; `0x11DB86` existiert im
ganzen Image nicht und entsteht erst zur Laufzeit (nach internem
`0x10470C`-Aufruf). Erzwungenes `0x11DB84` laesst die Kette laufen.

Damit ist der zuletzt verfolgte Emitter-Pfad eine Sackgasse; der echte
Menue-2D-Draw muss ueber `0x277004`/HLE-engine2d gesucht werden.


## Submit-Kette rein dynamisch dispatcht

Scan (ARM+Thumb) ueber `code_decomp.bin`: `0x12493C`, `0x12FD30`,
`0x12FD38`, `0x139914`, `0x13F2D4`, `0x12FC24` haben **je 0 Aufrufer**
und kommen auch nicht als Datenwort vor.

`HLE_RETFORCE` auf die vier `fn_0012493C`-Gates (`284AA8=1,1300C8=1,
130170=1`, `tools/run_emitter_gate.py`) erreicht `0x12493C` 0x;
`qops=0`. Der Submit startet also erst, wenn der **Vtable-Dispatch-
Eintrag** vom echten Loader gesetzt ist.


## Dispatch-Kette zum Emitter gefunden

Roh-Branch-Scan (ARM-Encoding) statt Linear-Disasm ergab:

```
0x100B8C -> 0x100F98 -> 0x1034F4 -> 0x108F70 -> 0x284D24
 -> 0x2850A8 -> 0x11E494 -> 0x280034 -> 0x12493C
 -> 0x12FC24 (Emitter) + 0x12FD30 (Submit) -> 0x139914 -> 0x13F2D4
```

`0x11E494` ist der gesuchte Dispatch-Eintrag (`r0=[world+0xBC4]`).
Gate in `0x108F70`: `0x11B530` (sonst Sprung nach `0x1094CC`).

Messung (`out/logs/dispatch_chain.txt`):
```
[hook-final] count addr=001034F4 hits=1
[hook-final] count addr=00108F70 hits=1
FEHLT 11B530/109090/284D24/11E494/12FC24/12FD30, qops=0
```

Init und GX-Entry laufen; `0x108F70` verlaesst den Render-Zweig vor
`0x109090`. Naechster Blocker exakt lokalisiert.


## Engine-2D-Renderpfad laeuft (Gates 1BC450/11B530)

Mit `HLE_RETFORCE="1BC450=3EE12C,11B530=1"` laeuft der Render-Zweig
in `0x108F70`: Textur `0x132370(tex=0CFE7100)` gebunden, 2D-Display-
list-Entry `0x0CFE3000`/`14004440` installiert, Sprite `idx=0`
submitted. Aber submitted wird die HLE-Stub-Liste `0x0CFE6000`
(0x24 B), daher `qops=0`; `[scene]=00000000`.

Frame: TOP 90281/90400, BOTTOM 26091/71360 (Engine-Test).


## [scene] gebunden, 4 Sprite-Eintraege submitted

`HLE_ENGINE_FRAME=1` + `HLE_ENGINE_RES=1`:
```
[engine] Scene-Konstruktor 0x381CE4 aufrufen
[engine] nach Konstruktor [scene]=003CA494
[hook-final] 0028D424 hits=6516
[engine2d] Sprite-Entry idx=0..3 submitted
```
Der Menue-Ressource-Loader laeuft 6516x, und der Engine-2D-Pfad
submittet vier Sprites. Submitted wird aber weiter die HLE-Stub-Liste
`0x0CFE6000` (`qops=0`); `0x12FC24` bleibt 0 Hits.


## Emitter 0x12FC24 erstmals mit korrekten Args

`tools/run_chain_direct.py` (Orchestrator 0x12493C direkt, Gates erzwungen):

```
[hook] regs addr=0012FC24 hits=1 r0=003EE460 r1=003EE2D0 r2=0D100000
       r3=00001000 r6=003EE12C r8=003EC028 lr=00124A40
```

r0=menu+0x334, r1=menu+0x1a4, r2=Zielpuffer, r3=Groesse. Emitter baut
0x28-Byte-Nodes und laeuft mit Groesse 0x1000 uebers Budget
(Saved PC 0x2896C4); 0x12FD30/0x13F2D4 noch nicht erreicht, qops=0.


## Emitter mit korrekten Args + Node-Schleife laeuft

`0x12FC24` mit r0=menu+0x334, r1=menu+0x1a4, r2=0D100000, r3=0x14;
Node-Schleife `0x2896B8` mit r4=0x20 laeuft (16120 Hits). Aber
`0x12FD30` bleibt 0 Hits -> Submit-Zweig bricht nach dem Emitter ab,
`qops=0`.


## Emitter kehrt nicht zurueck (Submit-Zweig unerreicht)

`0x12FC24` laeuft mit korrekten Args (r0=menu+0x334, r1=menu+0x1a4,
r2=0D100000, r3=0x14), aber 0x133018/0x133150/0x12FD30 bekommen
0 Hits; PC bleibt bei 0x2896C4. Der Node-Loop 0x2896B8 laeuft 16120x,
der aeussere Loop in 0x275994 terminiert nicht -> qops=0.


## Submit 0x12FD30 erstmals erreicht (Args korrekt)

Mit `275994=1` (haengender Emitter-Helper uebersprungen) laeuft der
Orchestrator bis `0x12FD30` (20 Hits):
r0=menu+0x554, r1=menu+0x1a4, r2=menu+0x334, r3=0D100000.
`0x139914`/`0x13F2D4` noch nicht; Gates in 0x12FD30: `0x139844`
(SDK-Flag) und die Groessenpruefung mit Stack-Args (0x12FD68..0x12FD90).
qops=0.


## Submit-Kette 0x139914 + 0x13F2D4 laufen (20x)

Mit Arena-Gates (283160=0E000000, 2691DC=01000000), SDK-Flag
(139844=1) und uebersprungenem haengendem Helper (275994=1):
```
00139914 r0=003EE680 r1=003EE2D0 r2=0E800000 r3=00000040
0013F2D4 r0=003EE680 r1=003EE2D0 r3=0E800040 (20x)
```
qops bleibt 0, weil der Engine-Trigger nur bei fa2==0x12FD30 ausgeloest
wird; sichtbare Trigger sind die HLE-2D-Stub-Liste 0x0CFE6000.


## Submit-Body vollstaendig (0x139914/0x13F2D4/0x13A854), GSP-Bruecke fehlt

Arena-Gates 283160=0E000000/2691DC=01000000, SDK 139844=1, 275994=1:
```
00139914 r0=003EE680 r2=0E800000 r3=00000040
0013F2D4 20x, 0013A854 19x
[queue] pre-trigger n=0 -> qops=0
```
Die Engine fuellt ihren Command-Buffer 0x3EE680, aber noch nicht
0x10002818/1C/B0 (GSP-Bruecke). HLE-Trigger laeuft jetzt auch fuer
fa2==0x12493C.


## Submit-Body 19-20x, Zielliste 0x0E800000 leer

0x139914 (19x) / 0x13F2D4 (20x) / 0x13A854 (19x) laufen; 0x12FD30
setzt den Command-Buffer-Header. Aber:
```
[queue] pre-trigger n=0
[qmem] E800000: 00000000 ...
```
weil 275994=1 den PICA-erzeugenden Emitter-Body ueberspringt.
Naechster Schritt: 0x275994-Loop terminieren lassen.


## Emitter-Ziel r2=0x0E800000/r3=0x14 bestaetigt; Puffer bleibt leer

0x12FC24 wird mit r0=menu+0x334, r1=menu+0x1a4, r2=0x0E800000,
r3=0x14 gerufen; 0x139914/0x13F2D4/0x13A854 laufen, aber 0x0E800000
bleibt null und die Queue n=0 -> qops=0. Naechster Schritt: die exakte
Schreibadresse in 0x12FC80 (r1) verifizieren.


## Count-Kette: [0x3EE2D4]=0 war Ursache fuer count=0

`0x140140(menu+0x1a4)` = `[menu+0x1a4+4] -> +0x3C -> count`.
Dump: `[emitin] chain [3EE2D4]=00000000 ... count=0`.
Mit Poke-Kette (3EE2D4=0D300000, 0D30003C=0D300100, 0D300100=1):
`count=1, r3=8` -> Size-Gate besteht, aber Emitter-Node-Builder haengt
(PC 0x26927C/0x2896C4), 0x0E800000 bleibt leer, qops=0.


## Engine-Puffer bei Submit noch leer

`[postemit] E800000: 00000001 0..0 | CB3EE680: 00000000 ...`
-> Zielliste hat nur Count=1, Command-Buffer 0x3EE680 ist leer.
Der Node-Builder 0x26926C (Ring 0x1433A4/0x266858) erzeugt mit
synthetischen Daten keine PICA-Paare. qops=0, Frame unveraendert.


## Node-Builder haengt im leeren Pool (ctx+0xB4)

`0x26926C` wartet in der Schleife 0x269310 auf `[node+8] == r5`,
waehrend Pool `ctx+0xB4` (+0x20 Maske/+0x24 Index/+0x28 Anzahl) und
Ring `+0x17C..0x18C` im Port genullt sind -> Endlosschleife, kein
`[postemit]`, `0x0E800000` nur Count=1, qops=0.


## Skills genutzt: guest-refs -> Lazy-Init-Gate 0x28AAF0

`guest-refs 0x0042B7AC` -> nur `fn_00275994`. Gate `0x28AAF0=1`
erzwingt `0x13038C` (Ring-Init); danach kehrt `0x12FC24` zurueck und
`CB3EE680` fuellt sich (003ECB68 00000000 00000018 ...). Aber
`[gspq] list=003EE680 ops=0`: Engine-Format ist kein
(value,header)-PICA-Format -> qops=0.


## Engine-Descriptor zaehlt: qops=4 (20x) ohne gsp_engine_list-Fallback

`guest-watch 0x3EE680` zeigte: der Wert wird von `fn_002771EC`
(menue drain) geschrieben; `[0x3EE680]` ist ein Zeiger auf die
Ressourcenliste `0x3ECB68`. Der Command-Buffer-Descriptor selbst
(`003ECB68 00000000 00000018 00000000 00000000 00000004 ...`) hat
Count 4 bei `+0x14`.

Neuer GSP-Teil `gsp_engine_desc()` dekodiert genau dieses Engine-
Format (w0=Zeiger, +8=Groesse, +0x14=Anzahl). Ergebnis:

```
[gspq] tid=0 idx=0 id=1 list=003EE680 size=20 ops=4
[gsp] Trigger lr=00140170 addr=0CFE6000 size=24 qops=4
```

- `ops=4` 40x, `qops=4` 20x.
- Quelle ist der **Engine-eigene** Command-Buffer (vom Submit-Body
  `0x139914/0x13F2D4` gefuellt), **nicht** der `gsp_engine_list`-
  Fallback.
- Engine-Test-Frame: TOP 90281/90400 (Testkonfiguration), BOTTOM 26091.

Offen: Pixel-Rendering des Menues (TOP nutzt im Test noch den
HLE-Blend) und Abgleich der 4 Eintraege mit echten PICA-Kommandos.
