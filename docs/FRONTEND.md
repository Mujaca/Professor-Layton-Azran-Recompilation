# Host-Frontend (Fenster, Takt, Eingabe)

Alles hier betrifft nur den Host. Der Gastzustand wird ausschliesslich ueber
die echten HID-Eingaben beeinflusst; Takt, Fenster und Titelzeile lesen nur.

## Bildtakt

Der 3DS-VBlank laeuft mit **268111856/4481136 Hz** (59,8312 Hz), also einer
Periode von **4481136/268111856 s = 16,71368 ms**. Der Host erzeugt diesen
Takt selbst (`runtime/platform/frame_clock.h`):

* `QueryPerformanceCounter`/`QueryPerformanceFrequency` fuer die Zeitbasis,
  Zielzeit = Anker + `frame * 4481136 * freq / 268111856` (128-Bit-Ganzzahl,
  kein Drift durch aufsummierte Einzelframes).
* Warten mit `CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
  ...)`, zusaetzlich `timeBeginPeriod(1)` fuer den Fallback-Timer. Es wird
  **nicht** busy gewartet.
* Der VBlank-Thread in `runtime/kernel/hle.c` und die Anzeigeschleife in
  `runtime/main.c` benutzen je einen eigenen `FrameClock`.
* Haengt der Host laenger als vier Frames (z. B. Debugger), wird neu
  angekernt: keine Nachhol-Schwall, sondern stabiler Takt ab jetzt. Der Gast
  kann den Takt weder verlangsamen noch beschleunigen.

Test: `python tests/frame_clock_test.py` prueft die Ganzzahl-Taktberechnung
und die Fenstergeometrie (Sollwerte mit `Fraction` nachgerechnet).

## Fenster

* Beide Screens untereinander: oben 400x240, darunter zentriert 320x240
  (Kanvas 400x480).
* Skalierung **stufenlos** und seitenverhaeltnistreu; freie Flaeche bleibt
  schwarz (Letterbox). Verkleinern unter 1x mittelt (HALFTONE), Vergroessern
  ist naechster Nachbar.
* **Zwei Fenster:** `--split-screens` oder in `port.ini` `[display] layout =
  split` zeigt den oberen Screen und den Touchscreen in je einem eigenen
  Fenster (frei verschieb- und skalierbar, z. B. auf zwei Monitore).
  `--single-window` erzwingt das gemeinsame Fenster. Tastatur und Gamepad
  wirken, solange eines der beiden Fenster vorne ist; Schliessen eines
  Fensters beendet den Port. Intern bleibt der 400x480-Kanvas (Screenshots,
  `scene.py` unveraendert), nur die Anzeige wird aufgeteilt.
* **F11** schaltet randloses Vollbild des aktiven Fensters auf dem aktuellen
  Monitor und zurueck.
* Die Maus ueber dem unteren Screen wirkt als Touchscreen; die Umrechnung
  beruecksichtigt Skalierung und Letterbox.
* Titelzeile: `fps=` ist die tatsaechliche Host-Bildrate (gemessen ueber
  QPC), `Gast-Frames=` zaehlt die VBlanks, in denen das Spiel eine neue
  FrameBufferInfo veroeffentlicht hat (Diagnose, kein Gastzustand).

## Tastenbelegung: `port.ini` neben `port.exe`

Beim ersten Start legt der Port `port.ini` neben `out/port.exe` mit der
Standardbelegung an. Fehlt die Datei, gelten dieselben Standardwerte.
Format: Kleine INI, Abschnitt `[keys]`, je Zeile `3DS-Taste = Host-Taste`.
Unbekannte Namen werden ignoriert (Standard bleibt), `NONE` loest eine Taste.

```ini
[keys]
A = X
B = Z
X = S
Y = A
L = Q
R = W
START = Enter
SELECT = Backspace
UP = Up
DOWN = Down
LEFT = Left
RIGHT = Right
CIRCLE_UP = I
CIRCLE_DOWN = K
CIRCLE_LEFT = J
CIRCLE_RIGHT = L

[gamepad]
enabled = 1

[display]
layout = single
```

Tastennamen: ein Buchstabe oder eine Ziffer (`X`, `7`), oder `Enter`,
`Backspace`, `Tab`, `Escape`, `Space`, `Up`, `Down`, `Left`, `Right`, `Home`,
`End`, `PageUp`, `PageDown`, `Insert`, `Delete`, `Shift`, `Ctrl`, `Alt`,
`F1`..`F12`, `NONE`, ausserdem `0x<hex>` fuer einen Win32-Virtual-Key-Code.
Die Datei wird nur beim Start gelesen; Aenderungen brauchen einen Neustart.

## XInput-Gamepad (optional)

`[gamepad] enabled = 1` liest zusaetzlich das erste verbundene XInput-Pad
(`xinput1_4.dll`, sonst `1_3`, sonst `9_1_0`; per `LoadLibrary`, kein harter
Link). Wie die Tastatur wirkt das Pad nur, solange das Spielfenster im
Vordergrund ist (sonst steuerte ein Controller alle laufenden Instanzen).
Belegung:

| 3DS | Gamepad |
|---|---|
| A/B/X/Y | A/B/X/Y |
| L/R | LB/RB |
| Start/Select | Start/Back |
| Steuerkreuz | D-Pad |
| Circle-Pad | linker Stick (Deadzone 7849, auf -0x9A..0x9A skaliert) |

Tasten werden mit der Tastatur ODER-verknuepft; der Stick ersetzt das
Tastatur-Circle-Pad nur, wenn er die Deadzone verlaesst.

## Mods (`[mods]`, `--mods`)

Mods sind standardmaessig **aus** (docs/MODDING.md). `port.ini`:

```
[mods]
# 1 = Mods aus mods/<name>/ laden
enabled = 0
# Wurzel relativ zum Arbeitsverzeichnis (Repo-Wurzel)
# dir = mods
```

Kommentare stehen auf eigenen Zeilen (keine Kommentare hinter Werten).

`--mods` / `--no-mods` ueberstimmen `enabled`, `--mods-dir ordner` den
Ordner. Die Mods-Wurzel liegt wie `out/romfs_image.bin` relativ zum
Arbeitsverzeichnis (Standard `mods/` in der Repo-Wurzel, nicht neben
`out/port.exe`), weil port.exe dort startet. `frontier.py` und `scene.py`
uebergeben immer `--no-mods` (scene.py: `--mods ordner` fuer Mod-Laeufe).
Beim Start meldet das Log `[mods] ...` (geladene Mods, Prioritaeten,
ersetzte Dateien und Archiv-Eintraege, Konflikte).
Lua-Mods (`mods/<name>/scripts/main.lua`, docs/MODDING.md Stufe B) laufen
mit denselben Schaltern; ihre Ausgaben stehen als `[mod:<name>] ...` im
Log. Sie duerfen Maus (auch ohne Klick), Host-Tasten lesen und den Zeiger
ueber den Spielfenstern aendern (Pfeil/versteckt/Fadenkreuz); mit
`--deterministic` sehen sie keine Maus und keine Taste. Beispiele:
`mods-examples/` (in den Mods-Ordner kopieren).

## Diagnose-CLI fuer Testlaeufe

```
out/port.exe [romfs] [--frames N] [--frames-guest N] [--shot datei.bmp] [--shot-every N]
             [--shot-from M] [--shot-guest] [--press taste@[g]frame[+h][:dauer[:x:y]],...]
             [--savedata ordner] [--renderer soft|hw] [--deterministic]
             [--mods | --no-mods] [--mods-dir ordner]
```

* `--deterministic` (M18): reproduzierbare Laeufe. Alle gastsichtbaren Uhren
  (svcGetSystemTick, Sleep/Timeouts, Kernel-Timer, HID-Abtastung, Y2R-Ende,
  VBlank, DSP-Frames) laufen auf einer virtuellen Zeit
  (`runtime/kernel/vclock.h`): 30 ns je Gast-Block, im Leerlauf Sprung zur
  naechsten Deadline. GX-Befehle laufen synchron, und der Gast steht nach
  jedem VBlank, bis die Fensterschleife Bild, Screenshots und Eingaben dieses
  VBlanks erledigt hat (Host-Frame = VBlank-Nummer). Zwei Laeufe mit
  denselben Argumenten liefern dieselbe Bildfolge - unabhaengig von Renderer
  und Rechnerlast. Der Lauf ist **nicht** an die Wanduhr gebunden (Fenster
  laeuft so schnell der Host kann, Ton nur per `HLE_AUDIO_WAV` sinnvoll).
  Echte Maus-/Tasten-/Gamepad-Eingaben werden ignoriert, nur `--press`
  erreicht den Gast; das Fenster erscheint ohne Fokus (stiehlt ihn nicht).
  Ohne den Schalter: Echtzeit wie bisher. `tools/scene.py` setzt ihn standardmaessig.

* `--savedata ordner` nimmt `ordner` statt `out/savedata` als
  Spielstand-Wurzel (gleiches Layout: `savedata/<program-id>/data/00000001/`,
  `sharedextsavedata/...`, Spiegeldateien wie `head.lt6`). Standard
  unveraendert. Gedacht fuer Fixture-Kopien (`tools/scene.py`), damit
  Testlaeufe keine echten Spielstaende ueberschreiben.
* `--frames-guest N` beendet nach N Gast-Frames (Zaehler `gast=`, siehe
  unten); `--shot` wird dann dort geschrieben. Zusammen mit `--frames` gilt,
  was zuerst eintritt (`--frames` als Notbremse, falls der Gast steht).

* `--frames N` beendet nach N Host-Frames (`--shot` allein setzt 600).
* `--shot datei.bmp` schreibt beim Beenden ein BMP des zusammengesetzten
  Bildes (400x480: oben 400x240, unten 320x240 ab x=40,y=240).
  `--shot-every N` schreibt zusaetzlich alle N Host-Frames
  `name_000600.bmp` (Szenenfolge dokumentieren); `--shot-from M` erst ab
  Host-Frame M (Flacker-Analyse mit `--shot-every 1`). Mit `--shot-guest`
  zaehlt `--shot-every`/`--shot-from` Gast- statt Host-Frames; das ist fuer
  den Software-/Hardware-Bildvergleich noetig, weil die Gastgeschwindigkeit
  der beiden Pfade auseinanderlaeuft (`[port] ... gast=`).
* `HLE_AUDIO_WAV=datei.wav` schreibt den erzeugten DSP-Ton zusaetzlich als
  WAV (32728 Hz Stereo); `HLE_LOG` meldet am Ende `host_dropped`/`underruns`.
* `HLE_HW_STATE=1` meldet im Hardware-Pfad jede neue Kombination der
  Output-Merger-/Stencil-/Fog-Register einmal als `[hwstate]` (reines Lesen,
  keine Zustandsaenderung) sowie alle 5 s `[texcache]` mit Textur-Uploads/s
  und Dekodierzeit.
* `--press` haelt eine logische Taste (`a`, `b`, `x`, `y`, `l`, `r`,
  `start`, `select`, `up`, `down`, `left`, `right`, `touch`) fuer `dauer`
  Host-Frames ab `frame` gedrueckt (Standard 10). `touch` nimmt zusaetzlich
  Koordinaten auf dem unteren Screen auf (`touch@9000:20:160:120`, Standard
  160,120); Werte werden auf 0..319 bzw. 0..239 begrenzt. Mehrere
  `--press`-Angaben werden aneinandergehaengt (hoechstens 64 Ereignisse).
* **Gast-Zeit:** Mit dem Praefix `g` zaehlen Beginn *und* Dauer in
  Gast-Frames (`touch@g1800:20:160:64`, `a@g2400`) - also in VBlanks, in
  denen das Spiel einen eigenen Frame veroeffentlicht hat
  (`hle_gsp_guest_frames`, Log `[port] ... gast=N`). Das ist unter Last
  robust: laeuft der Gast langsamer, wandert die Eingabe mit. Steht der Gast
  (Ladepause), bleibt eine laufende Eingabe gedrueckt, bis er weiterlaeuft.
  Das Spiel liefert in Menues/Szenen etwa 30 Gast-Frames/s (halbe
  Host-Rate). Beide Formen lassen sich mischen.
* **Standbilder:** Wartet das Spiel auf einem unveraenderten Bild auf eine
  Eingabe (Raetsel-Titel, Tutorial-Hinweis), veroeffentlicht es keine neuen
  Frames, und der Gast-Zaehler kriecht oder steht ganz - eine dort geplante
  `g`-Eingabe kaeme nie. Dafuer `taste@g<frame>+<h>[:dauer[:x:y]]`: Start
  `h` Host-Frames, nachdem der Gast-Zaehler `frame` erreicht hat; Dauer in
  Host-Frames (`touch@g5950+320:8:97:98`). Verankert am Spielfortschritt,
  die kurze Wartezeit danach unabhaengig vom Zaehler. Parser:
  `runtime/platform/press_spec.h`, Selbsttest in
  `tests/frame_clock_selftest.c` (`python tests/frame_clock_test.py`).
* Alle drei Optionen sind reine Diagnose: die Eingabe geht denselben Weg wie
  Tastatur/Maus (`hle_hid_set_input`), es wird kein Gastzustand gesetzt.

## Fensterausgabe (Doppelpufferung)

Das Fenster zeichnet Hintergrund, Bild und Statuszeilen in einen
Speicher-DC und uebertraegt ihn mit einem einzigen `BitBlt`
(`runtime/platform/window.c`, `draw`). Vorher wurde direkt ins Fenster
gezeichnet (erst schwarz fuellen, dann Bild) - der kurz sichtbare schwarze
Zwischenstand war das Flackern im Fenster (M13). Screenshots (`--shot`) lesen
den Bildpuffer vor der Fensterausgabe und zeigten es deshalb nie; die vom
Spiel erzeugte Bildfolge ist flackerfrei (600 aufeinanderfolgende Frames mit
`--shot-every 1`, kein A-B-A-Wechsel).
