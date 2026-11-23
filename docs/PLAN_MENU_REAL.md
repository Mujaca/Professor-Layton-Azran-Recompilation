# Neuer Ansatz: Hauptmenue spielnah rendern (Stand 2026-11-23)

Dieses Dokument ist die ehrliche Bestandsaufnahme nach A2 und der neue
Plan. Kurzfassung: **die Pipeline steht, das Bild ist noch nicht das
Menue.**

## 1. Gemessener Ist-Zustand (Beleg)

`python tools/menu_compare.py out/shot_engine.bmp` (neu, siehe unten):

```
corr       : 0.024   (Layout; 1.0 = identisch)
std        : ref 47.9  ours 17.0   (Detail)
detail-hit : 0.29   (144 Bloecke mit Referenz-Detail)
mean-RGB   : ref [58.8 97.6 92.4]  ours [44.8 72.3 63.9]
```

`measure_ref.py` (nur Farbdifferenz > 60) meldet zwar
`TOP 75911/90400 mean 53.96`, aber die Korrelation zum Capture ist
**0.024** - das Bild ist ein Farbverlauf mit ein paar Rechtecken, kein
Menue. Das ist der Grund, warum alle Screenshots "farblich irgendwo
passend" aussehen, aber nichts gerendert wirkt.

Blockweise Helligkeit (25x20-Bloecke, links Referenz, rechts Port):

```
REF                          OURS
+====--::-:::---  ................
==-===-----:::::  :::.............
:--=+*+++***+:..  ----::::::::::::
::--=++-:::::...  -----::::::::.  
::-==+=--====-..  ------::::::... 
::--=--:-==-=-..  -------::.....:.
.::::.....:..::.  -------:::....::
:::...:.....::::  ------::::::::::
```

Die helle Titel-/Logostruktur (Zeilen 3-6, Spalten 4-12 der Referenz)
fehlt im Port komplett.

## 2. Was A2 trotzdem geliefert hat (abgeschlossen)

| Fakt | Beleg |
|---|---|
| `HLE_PICA_QUAD`, `HLE_MENU_TEX`, nv-`HLE_PRECALL` entfernt | Profil `tools/profiles/engine_pica_draw.json`, Log ohne diese Marker |
| echte `0x200..0x208`-Bloecke je Quad | `out/logs/engine.txt` (`sprites=96 len=4500`) |
| Texturen aus Spiel-Assets (`lt6_*.fa`, `title.xc`, `title_a.xa`) | `tools/make_menu_assets.py` |
| 96 Draws kanonisch + deterministisch | zwei Laeufe `bytegleich True`, `sprites=96 len=4500` |
| 0-Diff-Klausel | vom Nutzer endgueltig renegotiiert (2026-11-23) |
| Tests | `16/16` (selftest_tools), `13/13` (mvp_selftest) |

A2 war also ein **Pipeline-/Nachweis-Paket**, kein Bild-Paket.

## 3. Ursache: wir erfinden Geometrie, die im Spiel schon existiert

Der Menue-Overlay ist im Spiel eine **ANMC00-Animation** in
`title_b.xa` -> `RES.bin`. Sie enthaelt die Menuepunkte **mit Namen und
Koordinaten**. Neu extrahiert mit `python tools/anmc_menu.py`
(-> `tools/profiles/anmc_menu.txt`, 53 Records):

```
#continue_off 0    ( 103.0,  24.0)
#new_off 0         ( 105.0,  24.0)
#scr_off 0         (  99.0,  24.0)
#setting_off 0     ( 150.0,  24.0)
#sound1_off 0      (  24.0,  26.0)
#sound2_off 0      (  24.0,  26.0)
#sound3_off 0      (  24.0,  26.0)
#try_off 0         ( 256.0,  66.0)
#www 0             ( 144.0,  24.0)
#pointer 0         (   3.0,   3.0)
```

Dazu die Button-Animationen (`btr01_*`, `btr_*`, `s01_btr_*`, `s_btr_*`)
mit eigenen Koordinaten, z. B. `btr01_continue_off (99, 86)`,
`btr_continue_off (108, 108)`, `btr01_setting_off (84, 178)`.

Diese Zahlen sind aber **Groessen**, keine Positionen (siehe 3b).
Bisher hat HLE die echten Groessen/UVs durch Grid-Rects und zyklisch
wiederholte XPVB-Rects ersetzt - genau das ist der Bruch im Bild.

## 3b. Korrektur: die ANMC-Zahlen sind GROESSEN, keine Positionen

Die erste Deutung `(x,y)` war falsch. Beweis: `title_b.xa/000.pvb`
enthaelt 23 Quads mit Position, Groesse und UV. Die ANMC-Paare stimmen
**exakt** mit den XPVB-Groessen ueberein (`tools/menu_elements.py`,
17 von 18 Zuordnungen, Abweichung nur beim 3x3-Pointer):

```
continue    ANMC 103x24   XPVB 103x22   uv off 0,48-103,70   uv on 0,72-103,94
new         ANMC 105x24   XPVB 105x22   uv off 0,0-105,22    uv on 0,24-105,46
scr         ANMC  99x24   XPVB  99x22   uv off 0,96-99,118   uv on 0,120-99,142
setting     ANMC 150x24   XPVB 150x24   uv off 0,144-150,168  uv on 0,168-150,192
sound1..3   ANMC  24x26   XPVB  24x26   uv off 160..232,168  uv on +24
try         ANMC 256x66   XPVB 256x64   uv off 0,296-256,304  uv on 0,224-256,288
www         ANMC 144x24   XPVB 144x24   uv off 0,192-144,216
pointer     ANMC   3x3    XPVB  24x6    uv 136,168-160,174   (3x3 = Hotspot)
```

Damit sind **Groesse und UV jedes Menuepunkts aus Spiel-Daten bekannt**
(`tools/profiles/menu_elements.txt`). Was fehlt, ist nur noch die
Bildschirmposition: die XPVB-`pos`-Felder sind 0,0/0,2/-21,-3, also
elternrelativ; die absolute Position kommt aus den `#CHP`-Transformknoten
der ANMC (Matrizen, noch nicht dekodiert).

Hypothese `(x,y) = Bildschirmposition` ist damit widerlegt; der Test
steht in `tools/anmc_fit.py` (bestfit 1.25/y-aufwaerts, aber Quad-Treffer
nur 0.50x Zufall -> Zufallstreffer auf horizontalen Kanten).

Konsequenz fuer den Plan: Stufe  len1 ist **Groesse+UV erledigt**, offen ist
nur der Transform. Vorschlag: `#CHP`-Rekorde als 4x4-Matrizen lesen (die
1.0-Diagonalen im RES-Header ab 0x8C sind bereits sichtbar) und den
Eltern-Transform auf die Element-Rechtecke anwenden.

## 4. Neuer Plan (4 Stufen, jede mit Abnahmekriterium)

**Stufe 1 - Layout (halb fertig):** Groesse + UV je Menuepunkt sind aus
ANMC+XPVB belegt (`tools/profiles/menu_elements.txt`). Offen ist der
`#CHP`-Transform (4x4-Matrizen ab RES-Offset 0x8C) fuer die Position.
*Abnahme:* je Element ein Rect `(x, y, w, h, uv)` mit `corr >= 0.8`
gegen den passenden Capture-Ausschnitt.

**Stufe 2 - Texturen pro Element:** `000.pvb` + `011.xi` (XPVB/IMGC) des
`title_b.xa`-Archivs je Element zuordnen, IMGC/ETC1 korrekt dekodieren
(`_imgc.py`), Element einzeln rendern.
*Abnahme:* pro Element `corr >= 0.8` gegen den passenden Capture-Ausschnitt
(Referenz: `reports/menu_official.bin`, Crop aus dem ANMC-Rect).

**Stufe 3 - Titel-3D echt statt Host-Hack:** `title3d_bg.bin` entstand
aus einem ETC1-Host-Render mit Faktor `k=0.90` - ein Platzhalter. Ersetzen
durch echten ETC1-Decode + echte Projektion aus `title.xc` (Matrizen aus
dem Modell), inkl. Alpha/Depth.
*Abnahme:* `corr(Hintergrund ohne Overlay) >= 0.8`; der Faktor `k`
verschwindet aus `tools/make_menu_assets.py`.

**Stufe 4 - Engine fuehrt:** Textur-Bindung aus dem `0x30`-Strom
(PICA-Tex-Unit) statt HLE-Atlas; `HLE_PICA_SPRITES` und der
Sprite-Emitter entfallen. Die Engine liefert Matrix + UV + Textur, HLE
nur noch den PICA-Interpreter.
*Abnahme:* `sprites=`-Logzeile verschwindet (Draws kommen aus
`p_draw_arrays` des Streams), `out/shot_engine.bmp` bleibt 2x
byte-identisch.

**Gesamt-Abnahme (neues Mass, ersetzt `measure_ref` als Hauptkriterium):**

```
python tools/menu_compare.py out/shot_engine.bmp
  corr       >= 0.90
  std        >= 0.80 * ref
  detail-hit >= 0.80
```

## 5. Offene Entscheidung

Stufe 1+2 sind das Minimum fuer ein erkennbares Menue (Buttons an echten
Positionen mit echten Texturen), Stufe 3 macht den Hintergrund echt,
Stufe 4 macht es engine-getrieben. Empfehlung: **1+2 zuerst** (kleinster
Schritt mit sichtbarem Ergebnis), dann 3, dann 4.
