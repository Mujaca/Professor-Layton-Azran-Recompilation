---
description: Arbeite autonom an einem klar umrissenen Ziel des Layton-Ports (Regeln aus AGENTS.md, Abnahme per tools/check.py).
---

# /goal – autonomer Arbeitsauftrag

**Ziel dieses Auftrags:**

$ARGUMENTS

## Übergeordnetes Projektziel

Das Hauptmenü von *Professor Layton and the Azran Legacy* soll **echt** laufen:
Der unveränderte Spielcode bootet natürlich ab `0x00100000`, die Engine
rendert das Menü selbst über gsp::Gpu/PICA in ihre Framebuffer, ohne
Workarounds (`tools/workarounds/*` leer, keine `WORKAROUND(`-Stellen in
`runtime/`). Dein Auftrag oben ist ein Schritt dorthin.

## Pflicht vor dem ersten Befehl

1. Lies `AGENTS.md`, `docs/ROADMAP.md`, `docs/WORKAROUNDS.md`, `docs/REFERENCE.md`.
2. Lade die Skills `3ds-port` und `3ds-hle`.
3. Miss den Ausgangsstand: `python tools/frontier.py` (notiere services/divergenz/patches).

## Arbeitsweise

* Hypothese → Beleg (Log, `tools/g3ds.py`, Azahar-Quelle unter `tools/azahar/src`) → kleinste korrekte Änderung → `python tools/build.py --no-gen` (bzw. `python tools/build.py`, wenn du `tools/recomp` oder `tools/workarounds` änderst) → `python tools/frontier.py`.
* Implementiere echte 3DS-Semantik nach Azahar/3dbrew. Keine Spieladressen in `runtime/`, keine Pokes, keine erzwungenen Rückgabewerte, kein Nachbauen von Bildern, keine neuen Einträge in `tools/workarounds/`.
* Workarounds **entfernen** ist ausdrücklich erwünscht, sobald die echte Ursache behoben ist.
* Ändere niemals: `AGENTS.md`, `.opencode/`, `tools/check.py`, `tools/frontier.py`, `reports/`, den Teil von `docs/ROADMAP.md` oberhalb von „## Frontier-Verlauf“.
* Führe **kein** `git commit`, `git checkout`, `git reset`, `git stash` oder Branch-Wechsel aus. Das macht der Orchestrator.
* Wegwerfskripte nur unter `scratch/` (eigene Unterordner). `scratch/night/` gehört dem Orchestrator: dort nur `last_summary.md` schreiben.

## Abschluss (immer, auch bei Misserfolg)

1. `python tools/check.py` ausführen (bei Änderungen an `tools/recomp` oder `tools/workarounds`: `python tools/check.py --full`).
2. Schreibe `scratch/night/last_summary.md`:
   * Zeile 1: Commit-Titel (max. 72 Zeichen, deutsch, Präfix wie `kernel:`, `gsp:`, `ipc:`)
   * danach: was geändert wurde, welche Belege, Frontier vorher/nachher, was offen bleibt.
3. Wenn du nichts Sinnvolles erreicht hast: Änderungen nicht zurückhalten, aber in der Zusammenfassung ehrlich „kein Fortschritt“ schreiben und die gewonnenen Erkenntnisse festhalten.
