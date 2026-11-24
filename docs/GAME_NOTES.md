# GAME_NOTES – kurze Projektnotizen

## M3: keine Draws – Draw-Builder wird nie aufgerufen (2026-11-24)

**Befund:** Im natürlichen Boot schickt das Spiel 415 PICA-Listen in 40 s
(beide Bildschirme im Wechsel, u. a. `0x2286E820`, `0x228B5820`), aber keine
enthält `0x22E`/`0x22F`. Die Listen sind reine Zustandssets: Rasterizer
`0x040..0x06E`, Uniforms `0x290/0x291` (VS) und `0x2C0/0x2C1` (GS),
Attributformate `0x201..0x226` = 0, Default-Attribute `0x232..0x235` für
Index 0..11, Topologie `0x25E`, Farbziel `0x110..0x11E`, danach
`0x010 = 0x12345678` (Finalize/P3D). Ein Draw-Template mit
`0x000F022F` steht als Literalpool in `0x001325A0..0x001325A8`; der es
schreibende Builder beginnt bei `0x001323FC` und wird im Boot **nie**
ausgeführt (`g3ds.py guest-watch --pc 0x1323FC`: 0 Treffer; kein statisches
`bl`/Zeiger auf die Adresse – Aufruf über ein Objekt/Vtable).

**Folge:** M3 ist GPU-seitig nicht erreichbar; das Spiel erzeugt noch keine
Zeichenbefehle. Die GPU liefert auch keinen Zustand, auf den die Listen
warten: Das Spiel liest keine GPU-Register (`ReadHWRegs` kommt nicht vor),
und MemoryFill/DisplayTransfer werden nur zurückgesetzt
(`0x40001C/0x40002C` Maske 0xFF, `0x400C18` Maske 0xFF00), nie mit echten
Daten ausgelöst. Offen ist damit die Objekt-/Szenenquelle (Asset-/Thread-
Strang), nicht der PICA-Parser.

## Software-Rasterizer: Performance-Stand (2026-11-24)

Ziel: beide Bildschirme (400x240 oben, 320x240 unten) mit 60 fps schaffen,
also zusammen 172.800 Pixel je 16,6-ms-Frame.

**Benchmark:** `python tools/test_pica_c.py --bench` (baut
`tests/pica_selftest.c` und startet es mit `PICA_BENCH=1`). 2000 texturierte
Dreiecke (40x25 Quads) fuellen 400x240 genau einmal; ein Draw ueber 6000
Vertices, 5 Varianten: `tex` (RGBA8), `solid` (ohne Textur), `tex+light`
(Fragment-Lighting mit einer Lichtquelle), `etc1a4` (Texturformat 13) und
`tex+depth` (D16, LessThan, Depth-Write). Gemessen wird die Wanduhrzeit je
vollstaendiger PICA-Kommandoliste (2 Warmlauf-Frames, dann Minimum aus 8).
Ohne `--bench` ist die Testausgabe unveraendert.

**Vorher/Nachher** (gleiche Maschine; zeitweise parallele Host-Last, daher
die Spannweite):

| Variante | vorher (Mittel 5) | nachher (Mittel 5) | nachher (min 8) |
|---|---|---|---|
| tex (RGBA8) | 9,7 ms (103 fps) | 5,45 ms (183 fps) | 5,1 ms (195 fps) |
| solid | 9,5 ms (105 fps) | 4,55 ms (220 fps) | 4,4 ms (227 fps) |
| tex+light | 15,9 ms (63 fps) | 10,9 ms (92 fps) | 10,7 ms (94 fps) |
| tex+depth | 9,9 ms (101 fps) | 5,25 ms (191 fps) | 5,0 ms (199 fps) |
| etc1a4 | – | 5,82 ms ohne / 5,20 ms mit Cache | 5,3 ms (190 fps) |

Damit liegt der 2D-/UI-Pfad (Textur + TEV, kein Lighting) fuer **beide**
Bildschirme zusammen bei ca. 10,3 ms und schafft 60 fps. Der voll beleuchtete
3D-Pfad braucht fuer einen Screen 10,7 ms; zwei solcher Screens wuerden das
Budget reissen. Naechster Hebel ist `p_compute_fragments` (Lichtschleife je
Pixel), danach das Per-Pixel-Herausziehen der TEV-Konfiguration.

**Was geaendert wurde** (`runtime/gpu/gpu.c`, alles ohne Semantikaenderung –
alle PICA-Tests bleiben bit-identisch):

1. **Kantenfunktionen inkrementell** (`p_process_tri`): `w0..w2` werden je
   Zeile einmal exakt berechnet und je 16er-Pixelschritt um
   `-16*(y2-y1)` fortgeschrieben. Ganzzahlig exakt, also identisch zur
   Neuberechnung (Azahar rechnet zwar neu, das Ergebnis ist dasselbe).
2. **Bounding-Box/Scissor** bleibt wie in Azahar, aber Framebuffer-Groesse,
   Scissor-Modus und Kanten-Bias werden einmal je Dreieck statt je Pixel aus
   den Registern gelesen.
3. **Frueher Depth-Abbruch**: Tiefe haengt nur von den Baryzentren ab und
   wird deshalb vor Textur/TEV berechnet. Ist der Depth-Test aktiv und
   Stencil aus, wird rein lesend verglichen (`p_depth_prepass_ok`) und bei
   Fehlschlag der Pixel verworfen. Alpha-Test-, Fog- und Stencil-Reihenfolge
   bleiben unveraendert (Stencil schreibt bei Fehlschlag und nutzt weiter den
   regulaeren Pfad).
4. **Ungenutzte Arbeit uebersprungen**: Quaternion/View werden nur bei
   aktivem Lighting interpoliert, UV nur fuer aktive Textur-Einheiten (oder
   wenn die prozedurale Textur sie als Quelle waehlt), `tc0_w` nur fuer die
   Texturtypen 1..4, deaktivierte Einheiten werden nicht gesampelt.
5. **TEV-Durchreichstufen**: Eine Stufe mit `Replace(Previous)`, ohne
   Modifier, Skalierung 1x und ohne Combiner-Buffer-Schreibzugriff ist
   wirkungslos und wird uebersprungen (Stufe 0 ausgenommen, dort ersetzt die
   Hardware Previous durch Quelle 3). Das trifft die 5 Folgestufen der
   ueblichen `emit_tev0`-Konfiguration und viele Spiel-Listen.
6. **Farbziel-Fastpath**: LogicOp Copy ohne Blending und mit voller
   Kanalmaske schreibt das TEV-Fragment direkt, ohne das Ziel zu lesen.
7. **Textur-Kachel-Cache** (`p_texcache_*`): 8x8-Kacheln werden einmal als
   RGBA8 dekodiert (64 Texel) und nur fuer ETC1/ETC1A4 benutzt, weil dort die
   Dekodierung teurer ist als der Lookup. Gueltigkeit: beim Kachelwechsel
   `memcmp` gegen eine Kopie der Quellkachel; je Draw wird die Kurzpruefung
   zurueckgesetzt. Damit erkennt der Cache jede Speicheraenderung (Upload,
   DisplayTransfer, CPU-Schreibzugriff) ohne Hash-Kollisionsrisiko. Grosse
   Texturen (> 4 MiB Cache) und Sonderfaelle (Cube-Face-Adressen, Breite/
   Hoehe nicht durch 8 teilbar) fallen auf die direkte Dekodierung zurueck.
   Test: `tests/pica_selftest.c::test_texcache_etc1` (zwei Draws, dazwischen
   wird der ETC1-Block im Gastspielspeicher geaendert; Erwartung (255,8,8) ->
   (8,8,8) mit unveraendertem Alpha 0x77).

**Offen:** Kachel-Parallelisierung mit Threads (deterministisch nur mit
Kachel-Bins und fester Band-Reihenfolge; lohnt erst, wenn der
Lighting-Hotspot beseitigt ist), Mipmaps, sowie eine TEV-/Texturzustands-
Struktur je Draw statt Registerlesen je Pixel.
