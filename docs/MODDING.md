# Modding – Entwurf (Arbeitspaket 5)

Stand 2026-11-26, vom Nutzer als Entwurf festgehalten. Noch kein
Meilenstein; Abnahmekriterien legt der Mensch in `docs/ROADMAP.md` fest.
Anleitung fuer Mod-Nutzer und -Autoren (englisch):
[`MODDING_GUIDE.md`](MODDING_GUIDE.md).

## Grundsätze

* Mods sind eine **Nutzerfunktion**, kein Mittel gegen Runtime-Fehler. Die
  Verbote aus `AGENTS.md` (Gastspeicher fälschen, Spielcode patchen) gelten
  für die Runtime unverändert; ein Mod darf nie einen Fehler des Ports
  verdecken.
* Mods sind **standardmäßig aus**: `port.ini` `[mods] enabled = 1` schaltet
  sie ein, `--no-mods` ab (überstimmt die ini). Gate, Frontier, `scene.py`
  und SW/HW-Vergleiche laufen immer mit `--no-mods`.
* Die Mod-Schnittstelle in der Runtime ist **generisch**: keine
  Spieladressen, keine Dateinamen des Spiels. Spielspezifisches steht nur in
  den Mods selbst.
* Das Repo ist öffentlich: **keine Spielinhalte** (Texte, Grafiken, Skripte
  aus der ROM) in Mods unter Versionskontrolle. Test-Mods werden aus dem
  eigenen Dump erzeugt (Rezept im Repo, Ergebnis in ignorierten Pfaden).

## Befund (Grundlage des Entwurfs)

* Das Spiel öffnet sein RomFS über fs:USER `OpenFileDirectly` (SelfNCCH) und
  liest danach nur **Offsets** in die Level-3-Ebene; Dateitabellen
  (Hash/Meta, siehe `docs/REFERENCE.md`) wertet es selbst aus. Die Runtime
  sieht keine Dateinamen.
* Fast alle Inhalte (Skripte, Texte, Grafiken, XPCK-Pakete) liegen in drei
  Level-5-`ARC0`-Archiven (`lt6/arc/lt6_a.fa`, `lt6_b.fa`, `lt6_uk.fa`);
  Tabellen und Namen sind Level-5-komprimiert (LZSS/Huffman, siehe
  `tools/fa_index.py`). Musik (`.bcstm`) und Filme (`.moflex`) liegen als
  einzelne RomFS-Dateien vor.
* Der Recompiler erzeugt **eine C-Funktion je Gastfunktion**
  (`fn_XXXXXXXX`); direkte und indirekte Aufrufe betreten sie immer am
  Funktionsanfang.
* Gastcode läuft auf einem Host-Thread je Gast-Thread, aber nur der Halter
  des Staffelstabs führt aus. Im VBlank-Zweig von `sched_service` sind alle
  anderen Gast-Threads geparkt (sicherer Punkt für Gastspeicher).

## Ordnerstruktur

```
mods/
  <name>/
    mod.ini            # name, version, author, priority (höher gewinnt)
    romfs/<pfad>       # ersetzt ganze RomFS-Dateien
    fa/<archiv>/<pfad> # ersetzt einzelne Einträge in einem .fa-Archiv
    scripts/main.lua   # Lua-Einstieg (Stufe B)
```

Ändern zwei Mods dieselbe Datei, gewinnt die höhere Priorität; der Konflikt
wird ins Log geschrieben.

## Stufe A – Dateien ersetzen

* **A1 RomFS-Overlay (LayeredFS-Prinzip):** Beim Start baut die Runtime die
  Level-3-Tabellen (Hash, Meta, Datenoffsets) im Speicher neu auf. Lesezugriffe
  auf die virtuelle Ebene werden je Offsetbereich auf das Original-Abbild oder
  auf die Mod-Datei abgebildet. Referenz zum Lesen: Azahar `LayeredFS`.
* **Modkit:** `tools/modkit.py` listet und entpackt RomFS-Dateien und
  `.fa`-Einträge aus dem *eigenen* Dump (ersetzt die Abhängigkeit von
  fremdem `xfsatool`).
* **A2 Archiv-Overlay:** dasselbe eine Ebene tiefer: ein `.fa`-Archiv wird
  virtuell neu zusammengesetzt, wenn Mods Einträge darin ersetzen. Zuerst zu
  klären: akzeptiert das Spiel unkomprimierte Tabellen/Einträge? Sonst wird
  ein Level-5-Kompressor gebraucht.
* **Test-Mod:** ändert sichtbar einen Dialog (der erste Dialog nach dem
  Laden des Test-Spielstands). Erzeugt per Rezept aus dem eigenen Dump; der
  Nachweis läuft deterministisch (`scene.py`) mit und ohne Mod.

### Stand Stufe A (2026-11-26, Branch mods-overlay)

Umgesetzt: A1, Modkit, A2, Test-Mod. Code: `runtime/kernel/romfs_overlay.c`
(generisch, keine Spielnamen/-adressen), Anbindung in `hle.c`
(`OpenFileDirectly` SelfNCCH liefert bei aktiven Mods eine virtuelle Datei,
`Read` liest per `rfo_read`), Schalter in `main.c`/`port.ini`
(docs/FRONTEND.md, "Mods").

**Virtuelles Level-3 (A1).** Beim Start werden die Original-Metatabellen
eingelesen (Baum), Mod-Dateien eingehaengt und Hash-/Metatabellen in
Azahar-Reihenfolge neu gebaut (Bucket-Anzahl nach 3dbrew). Layout:
`[Kopf + Tabellen (Speicher)] [Datenbereich des Originals, unveraendert]
[ersetzte/neue Dateien, je 16 Bytes ausgerichtet]`. Unveraenderte Dateien
behalten ihren relativen Datenoffset; gelesen wird ueber eine sortierte
Extent-Liste (Speicher / Abbild / Host-Datei / Nullen). Ohne wirksame
Mod-Datei bleibt das Overlay inaktiv (Original-Pfad). Mod-Pfade werden ohne
Gross-/Kleinschreibung zugeordnet; neue Dateien und Ordner sind erlaubt.
Reihenfolge: `priority` aus `mod.ini` aufsteigend, bei Gleichstand nach
Ordnername; die spaeter angewandte (hoehere) Mod gewinnt, Konflikte stehen
im Log (`[mods] Konflikt ...`). Mit dem Dump und nur einem ersetzten
Archiv-Eintrag ist der neu gebaute Metadatenblock gleich gross wie das
Original (0xDD80).

**Archiv-Overlay (A2).** `fa/<RomFS-Pfad des Archivs>/<Eintrag>`; das
laengste Pfadpraefix, das eine RomFS-Datei ist, bezeichnet das Archiv. Das
ARC0 wird virtuell neu zusammengesetzt: Kopf, Verzeichnis-/Hashtabelle und
Namen bytegleich, nur die Dateitabelle wird dekomprimiert, Offset/Groesse
der ersetzten Eintraege umgeschrieben und **unkomprimiert** (Methode 0)
abgelegt; danach Namen-/Datenoffset im Kopf angepasst. Datenbereich des
Originals unveraendert, ersetzte Eintraege angehaengt (16 Bytes
ausgerichtet). Namen, CRCs und Reihenfolge bleiben, die Suche des Spiels
(nach CRC) trifft unveraendert. Neue Eintraege kann A2 nicht anlegen
(geloggt und ignoriert).

**Formate (am Dump geprueft, Referenz Kuriimu2/xfsatool nur gelesen).**
Level-5-Kompression: u32-Kopf = Rohgroesse << 3 | Methode (0 keine, 1 LZ10,
2 Huffman 4 Bit mit niedrigem Nibble zuerst, 3 Huffman 8 Bit, 4 RLE). ARC0:
Kopf 0x48; Tabellenmethoden in allen drei Archiven Verzeichnisse 2, Hash 0,
Dateien 3, Namen 1; Verzeichnis-Eintrag 20 Bytes {crc, u16 erstes
Unterverz., u16 Anzahl, u16 erste Datei, u16 Anzahl, u32 Namensbasis, u32
Verzeichnisname}, Datei-Eintrag 16 Bytes {crc32(Name), Namensoffset relativ
zur Basis, Datenoffset relativ zum Datenbeginn, Groesse}; alle 5539
Eintraege der drei Archive so aufloesbar. `tableChunkSize` (Kopf 0x20) = Summe
der auf 4 aufgerundeten Rohgroessen + 0x20 (bleibt beim Overlay gleich).
XSCR (`.xs`): Kopf 0x14 ('XSCR', u16, u16, u32, u32 Beginn Abschnitt 1 / 4,
u32 Beginn Abschnitt 2 / 4), drei Level-5-Bloecke, Abschnitt 2 = Texte
(alle 1318 `.xs` der drei Archive so zerlegbar; Methode 0 kommt dort 411-mal
als Abschnitt vor).
(`tools/fa_index.py` dekodierte Huffman 4 Bit bisher mit hohem Nibble
zuerst und unterschied Methoden nicht nach dem Kopf; behoben.)

**Experiment unkomprimierte Tabelle.** Hypothese: das Spiel akzeptiert eine
ARC0-Dateitabelle mit Methode 0. Versuch: Mod ersetzt einen Eintrag durch
eine bytegleiche Kopie (Overlay aktiv, Tabelle Methode 0, Eintrag
verschoben), Szene `froenborg` deterministisch. Ergebnis: Spiel laeuft,
Dialog erscheint mit Originaltext - **akzeptiert, kein Kompressor noetig**.
Die Bildfolge weicht ab g3350 leicht ab (3D-Szene, kleine Phasen-
verschiebung), weil die virtuelle Zeit an ausgefuehrte Gast-Bloecke
gekoppelt ist (`VCLOCK_NS_PER_BLOCK`) und das Dekomprimieren einer
Methode-0-Tabelle weniger Bloecke kostet. Kontrolle: zwei Laeufe ohne Mod
bildgleich (36/36). Bildvergleiche mit Mods deshalb immer gegen einen
Kontroll-Mod mit gleichem Layout.

**Test-Mod und Nachweis.** `python tools/modkit.py make-testmod` schreibt
`mods/testmod-dialog/fa/lt6/arc/lt6_uk.fa/txt/uk/01/01_010020.xs`: im
Textabschnitt wird der erste sichtbare Textlauf durch `** MODKIT TEST **`
(mit Leerzeichen auf gleiche Laenge) ersetzt, Abschnitt unkomprimiert.
`--control` erzeugt dasselbe mit Originaltext. Szene `froenborg`
(`--every 50 --every-from 3000 --extra 300`) mit Kontrolle gegen Test-Mod:
36 Bilder, 25 bytegleich; die 11 abweichenden (ab g4250, Dialog sichtbar)
unterscheiden sich nur in x 24..350, y 169..184 = erste Zeile der
Dialogbox. Mods aus (`--no-mods`): Bildfolge bytegleich zum master-Build
(36/36). Gate-Test mit synthetischen Daten: `tests/romfs_overlay_test.py`.

Offen: A2 kann keine Eintraege hinzufuegen; Texte gleicher Laenge sind
erprobt, laengere Texte brauchen Wissen ueber die Offsets in Abschnitt 1
(XSCR); Mod-Dateien werden beim Start eingelesen (Aenderung = Neustart).

## Stufe B – Lua

* Lua 5.4 als Quellcode unter `third_party/lua` (MIT), mit zig mitgebaut.
  Download nur mit Zustimmung des Nutzers.
* Eine VM; Aufruf einmal pro VBlank im `sched_service`-VBlank-Zweig (Gast
  geparkt), in Echtzeit- und deterministischem Modus.
* **Sandbox:** kein `io`, kein `os`, kein `package.loadlib`/`require` von
  DLLs, kein `debug`. Zeit und Zufall nur aus der Gastzeit (Mods bleiben
  deterministisch).
* API (erster Wurf):
  * `on_load(fn)`, `on_frame(fn)`
  * `mem.read_u8/u16/u32/f32(addr)`, `mem.write_*(addr, v)` über die
    geprüften Zugriffe (`arm_mem_read`/`arm_mem_write`)
  * `input.get()`, `input.set{...}` – Tasten/Touch vor `hid_poll`
  * `host.mouse()` – Mausposition im unteren Screen auch ohne Klick, Tasten
  * `host.set_cursor("hidden"|"arrow"|"crosshair")`
  * `draw.text`, `draw.rect` – Host-seitiges Overlay, nicht im Gastbild
    (Screenshots unverändert); erst nach M17 (Swapchain-Ausgabe)
  * `log(...)` – in die Logdatei

### Stand Stufe B (2026-11-26, Branch mods-overlay, ohne `draw`)

Umgesetzt: Lua 5.4.7 unveraendert unter `third_party/lua` (Herkunft,
Pruefsumme und gebaute Dateien: `third_party/README.md`), eine VM in
`runtime/modding/lua_host.c` (generisch, keine Spieladressen; testbar ohne
Gast ueber `MluaMemOps`/`MluaHostOps`). Anbindung: `hle.c` (Start in
`hle_init` nach dem RomFS-Overlay, `mods_frame` im VBlank-Zweig von
`sched_service` direkt nach `det_host_sync`, Ueberschreibung in
`hid_update_pad_locked`), `main.c` (Host-Funktionen), `window.c`
(Maus ohne Klick, Tasten nach Namen, Zeigerform). Schalter wie Stufe A
(`--mods`/`--no-mods`/`--mods-dir`, `[mods] enabled`); ein Ordner zaehlt als
Lua-Mod, wenn `scripts/main.lua` existiert. Reihenfolge wie Stufe A
(`priority` aufsteigend, Gleichstand nach Ordnername).

**Ablauf.** Beim Start (vor dem ersten Gast-Befehl) laeuft je Mod
`scripts/main.lua` in einer eigenen Umgebung, danach alle `on_load`.
Einmal je Gast-VBlank am sicheren Punkt (alle anderen Gast-Threads
geparkt, Stabhalter; mit `--deterministic` nach der Host-Synchronisation,
also nach `--press`) laufen alle `on_frame` in Mod-Reihenfolge. Lua laeuft
auf dem Host und fuehrt keine Gast-Bloecke aus: die virtuelle Zeit bleibt
unberuehrt.

**API** (Lua-Globals je Mod):

| Funktion | Bedeutung |
|---|---|
| `on_load(fn)` | `fn()` einmal nach allen Hauptchunks (nur im Hauptchunk aufrufbar) |
| `on_frame(fn)` | `fn(frame)` einmal je Gast-VBlank; `frame` = Gast-Frame (Bildwechsel des Spiels, bei 30 fps zwei VBlanks je Frame) |
| `mod.name`, `mod.priority` | Ordnername, Prioritaet aus `mod.ini` |
| `mem.read_u8/u16/u32/s8/s16/s32/f32(addr)` | Gastspeicher lesen (little-endian, `arm_mem_read`) |
| `mem.write_u8/u16/u32/f32(addr, v)` | schreiben (`arm_mem_write`, Wert auf die Breite maskiert) |
| `input.get()` | `{a, b, x, y, l, r, start, select, up, down, left, right, circle_x, circle_y, touch, touch_x, touch_y}` so, wie das Spiel sie bis zum naechsten VBlank sieht (Host-Eingabe + `--press` + bisherige `input.set`) |
| `input.set{...}` | dieselben Felder ueberschreiben, nur bis zum naechsten VBlank (nur in `on_frame`); Tasten `true/false`, `circle_*` -156..156, `touch_x` 0..319, `touch_y` 0..239 (geklemmt); spaetere (hoehere) Mods gewinnen je Feld |
| `host.mouse()` | `{x, y, inside_touch, down, over_window}`: Zeiger in Touchscreen-Pixeln, auch ohne Klick, einfaches und geteiltes Layout |
| `host.key(name)` | Host-Taste gedrueckt? Namen wie in `port.ini` (`"F5"`, `"A"`, `"SPACE"`, `"0x41"`); nur lesend, die Taste wirkt weiter normal |
| `host.set_cursor(m)` | `"arrow"`, `"hidden"`, `"crosshair"`: Zeiger ueber den Spielfenstern |
| `guest.frame()`, `guest.vblank()`, `guest.time_ns()` | Gast-Frame, VBlank-Zaehler (ab 1), Gastzeit seit Start (virtuell mit `--deterministic`) |
| `log(...)`, `print(...)` | Zeile `[mod:<name>] ...` ins Log |
| `require("name")` | `scripts/name.lua` desselben Mods (`a.b` = `scripts/a/b.lua`), gecacht |

Ungueltige Adressen (ausserhalb des Gastspeichers), falsche Felder oder
Tastennamen sind Lua-Fehler. Mit `--deterministic` ist `host.*` neutral
(keine Maus, keine Taste gedrueckt), wie die echte Eingabe dort auch.

**Sandbox.** Geoeffnet: base, string, table, math, utf8, coroutine. Nicht
gebaut: io, os, debug, package (`liolib.c`, `loslib.c`, `ldblib.c`,
`loadlib.c`). Entfernt: `dofile`, `loadfile`, `warn`, `string.dump`;
`load` nimmt nur Text (Standardumgebung = Mod), `require` nur Lua-Dateien
aus `scripts/` desselben Mods (Name aus `[A-Za-z0-9_.-]`, kein `..`).
Deterministisch: feste String-Hash-Saat (`LUA_DEFINES` in
`tools/build.py`), `math.random` fest geseedet (`math.randomseed()` ohne
Argument = 0), keine Uhr ausser `guest.time_ns()`. Nicht deterministisch
bleibt nur die `pairs`-Reihenfolge bei Tabellen/Funktionen als Schluessel
(Adressen) und `tostring` von Tabellen. Jeder Mod hat eigene Globals;
Bibliothekstabellen (`string`, `math`, ...) sind gemeinsam und sollen nicht
veraendert werden.

**Grenzen.** Instruktionsbudget 50 Mio. Lua-Instruktionen je Aufruf
(Hauptchunk, je `on_load`, je `on_frame`), Count-Hook alle 1000; ein
erschoepftes Budget fangen `pcall`, `xpcall` und `coroutine.resume` nicht
ab. Speichergrenze 256 MiB fuer die VM. Lange Musterlaeufe in C
(`string.find` mit Rueckverfolgung) zaehlen nicht als Instruktionen.

**Fehler.** Syntax- und Laufzeitfehler (auch Budget, Speicher) werden als
`[mod:<name>] Fehler: <Datei:Zeile: Meldung>` mit `stack traceback` geloggt;
danach ruft die Runtime keine Funktion dieses Mods mehr auf
(`[mod:<name>] deaktiviert`), das Spiel und die anderen Mods laufen weiter.

**Beispiele** (unser Code, keine Spielinhalte; `mods-examples/`, zum
Ausprobieren in den Mods-Ordner kopieren):

* `frame-log`: liest nur (Code-Wort am Programmbeginn, `input.get`, Zeit)
  und loggt alle 300 VBlanks; nutzt `require("fmt")`.
* `input-demo`: tippt ab Gast-Frame 2550 fuer 20 Gast-Frames auf "New Game"
  (160/64) wie `--press touch@g2550:20:160:64`; in Echtzeit zusaetzlich:
  F6 halten = Touchscreen an der Mausposition beruehren, Fadenkreuz ueber
  dem Touchscreen.

```
xcopy /E /I mods-examples\input-demo mods\input-demo
out\port.exe --mods
```

**Nachweis (deterministisch, Szene `titel`, `--every 50 --every-from 2000
--target 2900`, 19 Bilder inkl. Endbild; Bilder unter
`scratch/scene/titel-lua-*`):**

| Lauf | Ergebnis |
|---|---|
| `--press touch@g2550:20:160:64` ohne Mods (`lua-press`) gegen denselben `--press` mit `frame-log` (`lua-log`) | 19/19 bytegleich: ein lesender Mod veraendert die Bildfolge nicht |
| `input-demo` ohne `--press` (`lua-input`) gegen `lua-press` | 19/19 bytegleich: `input.set` wirkt genau wie das gleiche `--press` |
| ohne Mods und ohne `--press` (`lua-none`) gegen `lua-input` | 12/19 gleich, ab g2600 verschieden (Tipp auf "New Game" wirkt) |

Mit Mods, aber ohne Datei unter `romfs/`/`fa/`, bleibt das RomFS-Overlay
inaktiv (`[mods] keine wirksame Mod-Datei`); der Vergleich gegen `--no-mods`
ist deshalb ohne Kontroll-Mod zulaessig.

Gate-Test mit synthetischen Skripten: `tests/lua_host_test.py`.

## Stufe C – Funktions-Hooks

* Der Recompiler erzeugt am Anfang jeder `fn_XXXXXXXX` eine generische
  Prüfung `if (g_hook[idx]) mod_hook(c, idx)` (Tabelle ~13k Bytes, kein
  Spielwissen).
* Lua: `hook.before(addr, fn)`, `hook.after(addr, fn)`,
  `hook.replace(addr, fn)`; Register über `regs.r0` … Hooks laufen im
  Gast-Thread mit Staffelstab.

## Beispiel-Mods (QoL)

* **Lupe folgt der Maus:** Lupenmodus erkennen (Hook oder Speicherwert),
  Lupe auf `host.mouse()` setzen, Klick = kurzes Antippen, Systemzeiger nur
  in diesem Modus ausblenden. Adressen stehen nur im Mod.

## Reihenfolge

A1 → Modkit → A2 → Test-Mod (Dialog) → B (ohne `draw`) → C → Lupen-Mod →
`draw` nach M17.
