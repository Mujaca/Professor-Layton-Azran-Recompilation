# GAME_NOTES - Spielbeobachtungen (unveraenderter Spielcode)

Kurz halten. Nur belegte Beobachtungen aus Laeufen des natuerlichen Boots.

## Boot-Szenenfolge (Port, Host-Frames bei ~59,5 Hz)

| Zeit | Szene | Beleg |
|---|---|---|
| 0-10 s | Film-/Fade-Bild (grau, viele Farben); ein eigenes Level-5-Logo war nicht zu sehen | `scratch/m6b/evidence/film_10s.png` |
| 20-30 s | schwarz (Filmeende/Uebergang) | `scratch/m6b/runC_001200.bmp` |
| 40-110 s | gerenderte 3D-Szenen mit Fades (Kamera/Szene wechselt); 100-110 s blau-gruene Szene | `scratch/m6b/evidence/szene_50s.png`, `szene_100s.png` |
| ab ~110 s | **Titelbildschirm** (Logo-Band oben, Panel unten; je Lauf 110-120 s); Bildwerte nahe der Referenz: top mean (59,100,96) vs. `reports/azahar_topscreen.png` (56,92,88), bottom (17,103,89) vs. (14,80,69) | `scratch/m6b/evidence/titel_120s.png` |
| ab ~130 s | **Hauptmenue** (Buchseiten-Layout, vier Knoepfe unten), stabil bis 200 s | `scratch/m6b/evidence/hauptmenue_135s.png` |

Der Titel erscheint ohne Eingabe; die Gast-Framerate liegt in dieser Phase bei
~11 fps (Host ~60 Hz), die Szenen brauchen daher Wandzeit. Mit gehaltenem
Start/A bleibt der Titel stehen (Lauf `runB.log`, siehe unten).

## Eingabe

* **Touch auf dem unteren Screen** loest den Uebergang Titel -> Hauptmenue
  aus (`--press touch@7300:20`, Lauf `scratch/m6b/runC.log`: Titel bei 120 s,
  Uebergangsbild bei 125 s, Menue ab 130 s).
* **Start und A allein genuegen nicht** (Lauf `runB.log`):
  `start@7000:360` (6 s) und `a@7400:360` (6 s) halten den Titel bis 145 s
  unveraendert (top mean (59,100,96) identisch zum Titelframe).
* **Das Menue reagiert auf Touch**: `touch@8100:30:67:180` (zweiter Kasten
  von links in der unteren Knopfreihe) aendert danach den
  Seiteninhalt des unteren Screens dauerhaft (nur y 51..148 des unteren
  Screens, top unveraendert; 7461 Pixel heller, 5117 dunkler, also anderer
  Inhalt statt Fade). Beleg `scratch/m6b/evidence/menue_nach_touch_140s.png`
  gegen `hauptmenue_135s.png`, `scratch/m6b/evidence/menue_touch_diff.txt`.
* **Die uebrigen drei Kaesten (x=24/155/267, y=180) zeigen keine sichtbare
  Reaktion** (Lauf `runD.log`, je 30 Frames gedrueckt, Shots alle 5 s bis
  200 s: unterer Screen konstant mean (156,142,111);
  `scratch/m6b/evidence/knopfreihe.txt`).
* Der HID-Pfad ist in Ordnung, kein Workaround: `HLE_HID_LOG=1` zeigt bei
  gedrueckter Taste `state=00000001` und beim Touch
  `valid=1 pos=(67,180)`; das Spiel liest die Ringe (sonst waere keine
  Reaktion sichtbar). Layout/Takt gemaess `docs/REFERENCE.md` (HID) und
  Azahar `hid/hid.cpp`.

## Offen (Bildauffaelligkeit, keine Diagnose)

Die Seitenflaeche des Menues erscheint als sehr regelmaessiges Zellenraster
(Zellbreite ~22 px mit Randspalte). Ob das eine Textur (Papier/Kasten) oder
ein noch falsch gesampeltes/dekodiertes UI-Texturformat ist, ist nicht
geprueft; fuer den Beleg "Menue erreicht und bedienbar" ist es unerheblich.
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

**Nachtrag Ereignisprüfung (2026-11-24):** Ein 25-s- und ein 200-s-Lauf mit
`HLE_PICA_DIAG=1`, `HLE_KLOG=1` belegen, dass der Gast **nicht** auf GPU-
Ereignisse wartet. AcquireRight (`0x0016`) und
RegisterInterruptRelayQueue (Event 31) kommen je einmal; der GSP-Handler
(Thread 6, LR `0x00142620`) wird ~114x/s aus Event 31 geweckt und drainiert
die RelayQueue (PDC0/PDC1 je VBlank plus P3D der Listen). Die
FrameBufferInfo wird im VBlank übernommen (2522 `SetBufferSwap`-Latches in
25 s, Pufferwechsel `1F300000/1F000000` <-> `1F346500/1F038400`). Thread 0
läuft durchgehend im Software-Videodecoder `fn_0016E4A0` (~29 M Bloecke in
12 s), während die 14 Setup-Listen/Frame weiterlaufen. Die fehlenden Draws
sind also die noch laufende Video-/Szenenphase, kein verschluckter
Interrupt. Der Draw-Builder `0x001323FC` bleibt unbetreten.

**Dabei behoben:** `runtime/gpu/gpu.c` schrieb die TextureCopy-Felder nach
0x307/0x308/0x309; Azahar (`regs_external.h`, `DisplayTransferConfig`) legt
`texture_copy.size`/`input_size`/`output_size` nach 0x308/0x309/0x30A
(0x306 Trigger, 0x307 Pad). Die bisherige Belegung machte jede TextureCopy
zum No-op (size im Pad). Test: `tests/pica_selftest.c::test_gpu_mmio`
(Registerablage, 16-Byte-Zeilen mit Gap, PPF-Trigger).

## Draw-Gate: Renderziel-Registry bleibt leer (2026-11-24)

**Befund (belegt, kein Recompiler-Fehler):** Der Draw-Gate `0x0027B34C`
bricht ab, weil `[renderer+0xB4C] == 0` ist. Renderer ist das BSS-Objekt
`0x00408808`; der Flush `0x0013657C` uebernimmt dort `[ctx+0x24]`, mit
`ctx = [renderer+0xD6C] = renderer+0x214` (erster der beiden eingebetteten
Render-Kontexte `+0x214`/`+0x458`).

**Schreibpfade auf `ctx+0x24`:** Der Bind (`fn_0011CAC4`, Block `0x0011CB58`)
speichert das Ergebnis des Registry-Lookups
`fn_0027BC50(renderer+0xA0, key)` dorthin; die Unbind-Pfade
(`fn_0026A84C` -> `0x00274070`, `fn_0011C0C0` -> `0x0011C0F0`, `0x0012D9EC`,
`0x00134618`, `0x001BE694`) schreiben 0.

**Laufzeit-Belege (natuerlicher Boot, 600 Frames):**
* `ARM_MEMWATCH` auf `0x00408A40` (ctx0+0x24) und `0x00408C84`
  (ctx1+0x24): 328 bzw. 404 Schreibzugriffe, **alle Wert 0**.
* `[0x00409574] = 0x00408A1C` (ctx0 aktiv), `[0x00409354] = 0` (Gate).
* Registry `renderer+0xA0`: `[0x00408918]` (Array) = 0, `[0x00408920]`
  (Count u16) = 0; nach dem BSS-Nullen nur die leere Konstruktion.
* `ARM_WATCH=0x00134900` (Comparator des Lookups): 0 Treffer – bei Count 0
  kehrt `fn_0027BC50` ohne Vergleich zurueck.
* Der Lookup wird mit echten Target-Ids aufgerufen (CoSim-Dump am 64. Treffer:
  `key = 0x2D2F398D`), die Registry liefert dafuer trotzdem 0.

**Co-Simulation (`tools/cosim`):** `fn_0027BC50` laeuft mit dem Dumpzustand in
Unicorn identisch (Ausrichtung gefunden, keine Abweichung, Rueckkehr im
Count==0-Zweig). `fn_001ED65C` (setzt `[renderer+0xD6C] = renderer+0x214`)
laesst sich in Unicorn nicht abschliessen, weil der Folgepfad Speicher
ausserhalb der vier moeglichen Dump-Regionen liest (VRAM/TLS); das ist eine
Werkzeuggrenze, kein Recompiler-Beleg.

**Folge:** Der Bind scheitert an fehlender Eingabe, nicht an Semantik: die
GL-Objekt-Registry am Renderer ist im gesamten Boot leer. Sie wird von der
Szenen-/GL-Ressourcen-Initialisierung befuellt, die in der Intro-/Y2R-
Videophase (`docs/WORKAROUNDS.md` §6) nicht erreicht wird. Kein Workaround
eingebaut.

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

## Threads und Ton (2026-11-25)

* Einziger Gast-Thread auf dem SysCore (processor_id 1): Prioritaet 33,
  laut Verhalten der Sound-Thread (wartet auf den DSP-Pipe-Interrupt und
  fuellt die Stream-Puffer nach). Haelt der Software-Rasterizer die
  Gast-Sperre, verhungert er -> exakte Null-Luecken 35..140 ms in der
  Titelmusik (`HLE_AUDIO_WAV`). Behoben durch die Sperr-Abgabe waehrend
  GPU-Arbeit (`gx_run_inline` in `runtime/kernel/hle.c`).
* Audiopuffer liegen im Linear-Heap (FCRAM phys 0x20000000+x = virtuell
  0x14000000+x).
* **Musikaussetzer behoben (2026-11-25 nachmittags):** Ursache waren
  gebuendelt zugestellte DSP-Pipe-Interrupts (der DSP-Thread stellte nur per
  TryEnter zu; bei belegter Gast-Sperre kamen ~4 Frames als ein Interrupt,
  15..23 ms spaet, `HLE_WSTAT`). Seit der blockierenden Zustellung je Frame:
  Luecken 0/0/1 statt 10/8/12 je ~40 s Titelmusik (A/B je 3 Laeufe).
* Musik-Stream: je Kanal ein Ring aus 5 ADPCM-Puffern a 8 KB (14336 Samples,
  0,438 s) an festen Adressen um 0x2031xxxx; ein Umlauf dauert 2,19 s. Die
  verbleibenden Null-Luecken der Titelmusik (master 08b773d: 6..13 Luecken je
  ~40 s, 20..110 ms) kommen genau in diesem Takt -> das Nachfuellen kommt
  zu spaet, nicht der DSP-HLE (Filmton ist sample-genau, DSP gegen Azahar
  abgeglichen). Messung: `HLE_AUDIO_WAV` + Lueckensuche; Streuung zwischen
  Laeufen gross, daher immer >= 3 Laeufe vergleichen.
* Thread 14 (Prio 46) entpackt (Bitbaum-Schleife 0x270D20..0x270D84) und
  macht am Titel ~45 % aller Gast-Bloecke aus; Kandidat fuer den Stream-
  Nachschub.
* Sampling-Profil am Titel: ~95 % der Thread-Zeit in Wartefunktionen,
  Gastcode kaum sichtbar, Raster-Worker ~77 % untaetig -> Bildrate und
  Nachschub sind durch die kooperative Gast-Sperre begrenzt, nicht durch
  Rechenleistung. Erfolglos getestet (je A/B, schlechter als master):
  Sleep(0) statt Sleep(1) beim Wiedereintritt, Sperr-Uebergabe per Zaehler,
  Abgabe in hle_pump nur bei bereitem hoeher-prioren Thread, 16x groesseres
  hle_pump-Intervall.
* A/B-Messungen am 2026-11-25 nachmittags (HW-Renderer, Gast-Frames bei
  Host-Frame 1200, je 3 Laeufe, master ~790): SysCore-Thread ohne Sperre +
  CAS-Monitor ~705 (kein Gewinn), asynchroner GX-Worker ~684, praeziser
  SleepThread 0 (Boot-Haenger) bzw. 303. Sichtbare fps: SW 10, HW 20 (Spiel
  an VBlanks gekoppelt). SleepThread-Nutzer (60 s): tid 0 1702x 1 ms
  (lr 0x10208C), tid 8 4708x 1 ms, tid 9 8813x 1 ms, tid 10-12 je ~3000x 7 ms.
  Entwurf fuer den naechsten Schritt: docs/IDEAS.md "Scheduler nach
  Azahar-Vorbild".
