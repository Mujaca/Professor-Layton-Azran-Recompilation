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
* Skalierung **ganzzahlig** und seitenverhaeltnistreu; freie Flaeche bleibt
  schwarz (Letterbox). Beim Groessenziehen wird der groesste passende Faktor
  neu berechnet, mindestens 1.
* **F11** schaltet randloses Vollbild auf dem aktuellen Monitor und zurueck.
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
```

Tastennamen: ein Buchstabe oder eine Ziffer (`X`, `7`), oder `Enter`,
`Backspace`, `Tab`, `Escape`, `Space`, `Up`, `Down`, `Left`, `Right`, `Home`,
`End`, `PageUp`, `PageDown`, `Insert`, `Delete`, `Shift`, `Ctrl`, `Alt`,
`F1`..`F12`, `NONE`, ausserdem `0x<hex>` fuer einen Win32-Virtual-Key-Code.
Die Datei wird nur beim Start gelesen; Aenderungen brauchen einen Neustart.

## XInput-Gamepad (optional)

`[gamepad] enabled = 1` liest zusaetzlich das erste verbundene XInput-Pad
(`xinput1_4.dll`, sonst `1_3`, sonst `9_1_0`; per `LoadLibrary`, kein harter
Link). Belegung:

| 3DS | Gamepad |
|---|---|
| A/B/X/Y | A/B/X/Y |
| L/R | LB/RB |
| Start/Select | Start/Back |
| Steuerkreuz | D-Pad |
| Circle-Pad | linker Stick (Deadzone 7849, auf -0x9A..0x9A skaliert) |

Tasten werden mit der Tastatur ODER-verknuepft; der Stick ersetzt das
Tastatur-Circle-Pad nur, wenn er die Deadzone verlaesst.

## Diagnose-CLI fuer Testlaeufe

```
out/port.exe [romfs] [--frames N] [--shot datei.bmp] [--shot-every N]
             [--press taste@frame[:dauer[:x:y]],...]
```

* `--frames N` beendet nach N Host-Frames (`--shot` allein setzt 600).
* `--shot datei.bmp` schreibt beim Beenden ein BMP des zusammengesetzten
  Bildes (400x480: oben 400x240, unten 320x240 ab x=40,y=240).
  `--shot-every N` schreibt zusaetzlich alle N Host-Frames
  `name_000600.bmp` (Szenenfolge dokumentieren).
* `--press` haelt eine logische Taste (`a`, `b`, `x`, `y`, `start`,
  `select`, `touch`) fuer `dauer` Host-Frames ab `frame` gedrueckt
  (Standard 10). `touch` nimmt zusaetzlich Koordinaten auf dem unteren
  Screen auf (`touch@9000:20:160:120`, Standard 160,120); Werte werden auf
  0..319 bzw. 0..239 begrenzt.
* Alle drei Optionen sind reine Diagnose: die Eingabe geht denselben Weg wie
  Tastatur/Maus (`hle_hid_set_input`), es wird kein Gastzustand gesetzt.
