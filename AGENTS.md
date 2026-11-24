# AGENTS.md – Regeln für Agenten in diesem Repo

Projekt: *Professor Layton and the Azran Legacy* (3DS, EUR) als **rein statische
Recompilation** nativ auf dem PC. Eigener Recompiler (`tools/recomp`), eigene
Runtime (`runtime/`). Kein Emulator im Produkt.

Lies vor jeder Arbeit: dieses Dokument, `docs/ROADMAP.md` (Ziel + aktueller
Meilenstein), `docs/WORKAROUNDS.md` (bekannte Schulden). Fakten zu 3DS-Interna
stehen in `docs/REFERENCE.md`.

## 1. Das Ziel ist fest

* Ziel und Meilensteine stehen in `docs/ROADMAP.md`. **Nur der Mensch ändert
  sie.** Ein Agent darf Abnahmekriterien nicht umdeuten, abschwächen oder
  „renegotiieren“. Ist ein Kriterium unerreichbar: aufhören und berichten.
* Fortschritt = **Frontier-Metrik** (`python tools/frontier.py`): wie weit der
  *unveränderte* Spielcode im natürlichen Boot mit Azahars Referenzspur
  übereinstimmt. Screenshots/Pixel-Korrelationen sind **keine** Metrik, solange
  das Spiel seine Frames nicht selbst rendert.

## 2. Verbotene Mittel (gelten nie als Fortschritt)

1. **Gastspeicher fälschen**: feste Werte an Spieladressen schreiben (`POKE`),
   Rückgabewerte erzwingen (`RETFORCE`), Spielfunktionen mit erfundenen
   Argumenten direkt aufrufen (`PRECALL`, `FRAME_LOOP`).
2. **Bild nachbauen**: Quads/Texturen/Referenzbilder aus Host-Dateien zeichnen,
   die das Spiel nicht selbst über GSP/PICA geschickt hat.
3. **Spielcode patchen**, um einen Fehler der Runtime zu verdecken. Ein Fehler
   im Gast ist fast immer ein Fehler in Recompiler, Kernel oder Service.
   Ursache dort beheben.
4. **Adressspezifischer Code** in `tools/recomp/` oder in generischen
   Runtime-Teilen (`runtime/cpu`, `runtime/kernel`, `runtime/gpu`).

Ist ein Workaround kurzfristig unvermeidbar, darf er **nur** als Eintrag in
`tools/workarounds/code_patches.json` bzw. `runtime/workarounds.c` entstehen,
mit Begründung und einem Eintrag in `docs/WORKAROUNDS.md`. Ziel ist, dass
diese Liste schrumpft, nicht wächst.

## 3. Arbeitsweise

* **Eine Hypothese → ein Experiment → ein Ergebnis.** Vorher den Befund
  mit `tools/g3ds.py` (trace-diff, guest-refs, guest-watch, callgraph) belegen.
* Azahar (`tools/azahar`, Submodul) ist **nur Referenz**: Quellcode lesen,
  Referenzspuren aufnehmen, vergleichen. Kein Azahar-Code wird gelinkt.
  Übernommenes Wissen wird nativ in `runtime/` implementiert.
* Wegwerfskripte gehören nach `scratch/` (in `.gitignore`), nicht nach
  `tools/`. Neue Tools in `tools/` nur, wenn sie dauerhaft gebraucht werden
  und in `docs/TOOLS.md` stehen.
* Keine neuen Umgebungsvariablen als Schalter für Spielverhalten. Erlaubt
  sind nur Diagnose-/Log-Schalter (`HLE_LOG`, `HLE_TRACE`, …), die
  Gastzustand nicht verändern.

## 4. Definition of Done für jeden Commit

```powershell
python tools/check.py
```

muss grün sein (Recompiler-Selbsttests, Tool-Selbsttests, Build,
Frontier ≥ letzter Stand in `docs/ROADMAP.md`). Ein Commit pro abgeschlossenem
Schritt, nicht pro Notiz. Doku-Updates gehören in denselben Commit wie der Code.

## 5. Was dokumentiert wird

* `docs/ROADMAP.md` – nur der Mensch ändert Ziel/Meilensteine; der Agent trägt
  den gemessenen Frontier-Stand ein.
* `docs/REFERENCE.md` – **verifizierte**, allgemeine 3DS-Fakten (IPC, GSP,
  PICA, RomFS …). Keine Projekt-Tagebücher, keine Adresslisten von Experimenten.
* `docs/WORKAROUNDS.md` – jede Abweichung vom unveränderten Spielcode.
* Erkenntnisse über Spieladressen gehören als Kommentar an den Code, der sie
  braucht, oder in `docs/GAME_NOTES.md` (kurz halten).
