# Modding – Entwurf (Arbeitspaket 5)

Stand 2026-11-26, vom Nutzer als Entwurf festgehalten. Noch kein
Meilenstein; Abnahmekriterien legt der Mensch in `docs/ROADMAP.md` fest.

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
