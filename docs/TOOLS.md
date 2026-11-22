# Werkzeuge: messen, vergleichen, Hypothese, A/B-Test

Alle Werkzeuge liegen unter `tools/` und benutzen nur die Python-Standard-
bibliothek. `capstone` wird verwendet, wenn es installiert ist; `guest-refs`
faellt sonst auf einen Minimal-Decoder fuer LDR-Literal/STR-Offset/BL zurueck.
Die gemeinsame CLI ist:

```powershell
python tools\g3ds.py <unterkommando> [optionen]
```

Jedes Unterkommando gibt es zusaetzlich als eigenen Einstieg, z. B.
`python tools\guest_refs.py 0x40B4C4`.

| Werkzeug | Zweck | Kurzform |
|---|---|---|
| `trace-diff` | Azahar-Referenzlog gegen Port-Log vergleichen | `python tools\trace_diff.py ...` |
| `guest-refs` | LDR-Literal-, STR/LDR-Offset- und BL-Referenzen finden | `python tools\guest_refs.py ...` |
| `guest-run` | Boot/Port im Hintergrund messen, Until-Kriterium, Env-Matrix | `python tools\guest_run.py ...` |
| `guest-watch` | `ARM_WATCH` / `ARM_MEMWATCH*` / `HLE_PERF` einheitlich auswerten | `python tools\guest_watch.py ...` |
| `guest-exp` | A/B-Messungen als JSON registrieren, Duplikate vermeiden | `python tools\guest_exp.py ...` |

## Konventionen

* Pfade sind relativ zur Repo-Wurzel (`C:\Users\user\Documents\Repos\3DS`).
* Adressen akzeptieren `0x40B4C4`, `40B4C4` oder Dezimal `4240580`.
* Exit-Codes: `0` = Messung/Analyse gelaufen, `1` = kein Treffer bzw.
  Erwartung verletzt, `2` = Datei/Argument fehlt (klare `FEHLER:`-Zeile).
* Artefakte gehen ausschliesslich nach `tools/out/`:
  * `tools/out/runs/<zeit>_<label>_<pid>/` - stdout, stderr, `meta.json`
  * `tools/out/experiments.json` - Experiment-Registry
  * `tools/out/genmap.json` - Cache der Funktionsgrenzen
* Standardausgabe ist eine Zusammenfassung (max. ~20 Zeilen). `--verbose`
  zeigt Details, Kontext und vollstaendige Tabellen.
* `guest-run` setzt standardmaessig `HLE_SRV_NAMES=1` und `HLE_PERF=1`
  (per `--env NAME=WERT` ueberschreibbar).

## 1. trace-diff

Vergleicht den Azahar-Debuglog (Zeitstempel, `Service.SRV`, `Service.FS`, ...)
mit dem Port-Log (`[srvname]`, `[ipc]`, `[fs]`, `[gsp]`). Normalisiert werden
Zeitstempel, Session-/Handle-Nummern, PIDs und Thread-IDs. Ein Dienst gilt im
Port als vorhanden, wenn er per `[srvname]` **oder** per `[ipc]` genannt wird;
so zaehlen vorbefuellte Handles (z. B. `fs:USER`) nicht faelschlich als fehlend.

```powershell
# 1) Reproduzierbarer Referenzfall: Port-Log aus dem echten 2026-11-19-Lauf
python tools\g3ds.py trace-diff reports\azahar_trace.txt tools\fixtures\port_boot_20261119.log --table

# 2) Frischen Port-Lauf erzeugen und direkt vergleichen
python tools\g3ds.py guest-run --timeout 20 --until "srvname.*gsp::Gpu" --label today
python tools\g3ds.py trace-diff reports\azahar_trace.txt tools\out\runs\<...>\today.out.log

# 3) IPC-Kommandostrom statt Dienst-Acquisitions vergleichen
python tools\g3ds.py trace-diff reports\azahar_trace.txt tools\out\runs\<...>\today.out.log --kind ipc --verbose --json-out tools\out\diff.json
```

Erwartete erste Divergenz fuer den heutigen Port:

```text
Erste Divergenz: Referenz: srv:GetServiceHandle(boss:U) -> Port: fehlt
```

`--kind srv` (Default) vergleicht Dienst-Acquisitions, `--kind ipc` den
Kommandostrom, `--kind all` beides. Bei `--kind ipc` werden bekannte Cmd-IDs
auf Methodennamen abgebildet (z. B. `fs:USER 0x0802` -> `fs.openfile`) und
`srv.`-Kommandos ignoriert, weil das Referenzlog sie nicht mitschreibt.
`--table` zeigt die Referenz-Reihenfolge und ob der Port den Dienst irgendwo
nennt. Die Fixture `tools/fixtures/port_boot_20261119.log` ist ein echter
stdout-Mitschnitt; Herkunft steht in `tools/fixtures/README.md`.

## 2. guest-refs

Findet auf eine Gastadresse:

* `ldr rX, [pc, #imm]`-Literale und die zugehoerigen Literal-Pools,
* `str`/`ldr`/`ldrh`/`strh`/`ldm`/`stm` mit Immediate-Offset, wenn die
  Basis-Registerkonstante innerhalb der Funktion symbolisch aufloesbar ist
  (`str r0,[r4,#0x154]` mit `r4=0x40B370`),
* Funktionsgrenzen aus `build/gen_boot.c`,
* ARM-`BL`/`BLX`-Aufrufer.

```powershell
# 1) Akzeptanzfall: STR +0x154 und LDR auf 0x40B4C4
python tools\g3ds.py guest-refs 0x0040B4C4

# 2) Nur Literal-Pools (z. B. Callback-Zeiger 0x40B370)
python tools\g3ds.py guest-refs 0x0040B370 --kind lite --limit 40

# 3) Nur Stores, mit 3 Zeilen Kontext
python tools\g3ds.py guest-refs 0x0040B4C4 --kind store --verbose
```

Beim Akzeptanzfall stehen u. a. `0x00134850` (`str r0,[r4,#0x154]`) und
`0x0028F284` (`ldr r0,[r4,#0x154]`) in der Tabelle. `--kind lite|store|load|all`
filtert; `--json-out` liefert maschinenlesbare Ereignisse.

Grenze: Die Basis-Register werden linear innerhalb einer Funktion verfolgt.
Werte, die erst aus dem Speicher geladen werden, sind unbekannt; dann wird
bewusst kein Treffer geraten.

## 3. guest-run

Startet `out\boot.exe` (headless) oder `out\port.exe` (Fenster), sammelt
stdout/stderr getrennt, beendet nach Zeit oder Until-Regex und fasst zusammen:
IPC-Histogramm, srvname-Liste, Watchdog-Thread-PCs, Until-Treffer, Laufzeit,
Blockrate, Fatal-Zaehler, Queue-Maximum.

```powershell
# 1) 60-s-Lauf bis zum ersten boss:U (oder Timeout)
python tools\g3ds.py guest-run --timeout 60 --until "srvname.*boss:U" --label boss

# 2) Env-Matrix parallel: zwei Varianten, je 20 s
python tools\g3ds.py guest-run --matrix "HLE_RM_ADV=0|1" --timeout 20 --jobs 2 --label rm

# 3) Fenster-Port mit Screenshot-Argument
python tools\g3ds.py guest-run --exe port --arg out/romfs --arg=--shot --arg out\shot.bmp --timeout 20
```

Wichtige Optionen: `--env NAME=WERT` (mehrfach), `--until REGEX` (mehrfach),
`--arg WERT` (mehrfach), `--jobs N`, `--json-out PFAD`. Bei `--matrix` wird
eine Ergebnistabelle statt Einzel-Zusammenfassungen ausgegeben. Ein Lauf, der
das Until-Kriterium nicht erreicht, endet mit `Status: timeout`, aber Exit-Code
`0` (die Messung selbst ist gueltig).

## 4. guest-watch

Kapselt die Runtime-Schalter und vereinheitlicht die Ausgabe auf
`ART, ADRESSE, WERT, PC, LR, FUNKTION@PC, AUFRUFER@LR, THREAD`.

```powershell
# 1) Erwarteten Schreibzugriff pruefen; stoppt beim ersten Treffer
python tools\g3ds.py guest-watch --write 0x40B4C4 --until "\[mw\] 0040B4C4" --timeout 10

# 2) Lese- und Bereichs-Memwatch plus Performance
python tools\g3ds.py guest-watch --read 0x40B4C4,4 --range 0x10003000,0x40 --perf --timeout 10

# 3) PC-Watch mit Registern und Aufrufer
python tools\g3ds.py guest-watch --pc 0x00134818 --timeout 10 --verbose
```

`--write` setzt `ARM_MEMWATCH`, `--read` `ARM_MEMWATCHRD`, `--range`
`ARM_MEMWATCHRANGE`, `--pc` `ARM_WATCH`, `--mwval` `HLE_MEMWATCHVAL`.
Wird eine mit `--write` erwartete Adresse nie geschrieben, steht
`NIE GESCHRIEBEN: 0x...` in der Ausgabe und der Exit-Code ist `1`.

## 5. guest-exp

Jede A/B-Messung wird als JSON-Eintrag mit Hypothese, Envs, Metriken und
Commit-Hash gespeichert. Die Signatur aus Binary, Argumenten, Envs, Timeout und
Until-Kriterien verhindert doppelte Messungen. `--repeat N` prueft die
Determinismus-Felder (`status`, `until_hits`, `srvnames`, `fatal`); der
IPC-Zaehler kann direkt nach einem Until-Kill um ein Ereignis schwanken und
wird separat als `ipc_identical` ausgewiesen.

```powershell
# 1) Messung mit 3 Wiederholungen registrieren
python tools\g3ds.py guest-exp record --hypothesis "GSP-IRQ aendert boss:U nicht" --env HLE_GSP_IRQ=1 --until "srvname.*boss:U" --timeout 20 --repeat 3

# 2) Alle Experimente anzeigen
python tools\g3ds.py guest-exp list --limit 20

# 3) Vor einer neuen Messung pruefen, ob sie schon existiert
python tools\g3ds.py guest-exp check --env HLE_GSP_IRQ=1 --until "srvname.*boss:U" --timeout 20
```

`record` gibt bei einer bereits vorhandenen Signatur den alten Eintrag aus;
`--force` erzwingt einen neuen Lauf. `show <id>` zeigt den vollstaendigen
JSON-Eintrag. Registry: `tools/out/experiments.json` (per `--registry`
ueberschreibbar).

## 6. Selbsttest

```powershell
python tools\selftest_tools.py
```

Der Test prueft gegen verifizierte Fakten:

* `guest-refs 0x40B4C4` findet `0x00134850` (STR +0x154) und `0x0028F284` (LDR),
* `trace-diff` erkennt den fehlenden `boss:U`-Aufruf,
* `guest-run --until` liefert zweimal genau einen Treffer,
* `guest-watch` sieht den Schreibzugriff auf `0x40B4C4` in Thread 2,
* `guest-exp --repeat 2` ist deterministisch,
* fehlende Dateien liefern Exit-Code 2 mit `FEHLER:`,
* `docs/TOOLS.md` und der Skill `.opencode/skills/3ds-hle/SKILL.md` existieren.

## 7. where — Warteschleifen und Felder

`where` startet den Port mit `HLE_PERFMAP`, liest Block-Histogramm und
Rueckwaertskanten und nennt pro Thread die heisse Funktion, die
Spin-Schleife, die gelesenen globalen Felder und deren Schreiber
(statisch per `callgraph`, optional dynamisch per `ARM_MEMWATCH`).

```powershell
python tools\where.py --run --thread 2
python tools\where.py --perfmap tools\out\where_perfmap.txt --no-dynamic
```

Akzeptanzfall: `Thread 2 wartet in fn_0028F250 auf Feld 0x0040B4C4;
letzter Schreiber: fn_00134818 (Tick-Task Slot 2 der Tabelle 0x003D7C50)`.

## 8. callgraph — statische Programmdatenbank

Einmalig indexieren (`tools/out/callgraph.json`), dann abfragen:

```powershell
python tools\callgraph.py callers 0x28F250
python tools\callgraph.py literals 0x134818
python tools\callgraph.py writers 0x40B4C4
python tools\callgraph.py readers 0x40B4C4
python tools\callgraph.py strings "boss:U"
python tools\callgraph.py fn 0x28F250
python tools\callgraph.py stats
```

Der Index enthaelt Funktionen, BL/BLX-Kanten, LDR-Literale (Literal ->
Code-Stelle), basis-symbolisch aufgeloeste globale Loads/Stores und
String-Verwender. `--rebuild` erzwingt einen Neuaufbau.

## 9. diff_trace — Referenz ab einem Anker

```powershell
python tools\diff_trace.py --port tools\fixtures\port_boot_20261119.log
python tools\diff_trace.py --kind all --after "gsp.*001E" --limit 20
```

Behebt die alte Parser-Luecke: Azahar-Zeitstempel werden entfernt, daher
werden auch GSP/DSP/FS/Y2R-Methodenzeilen gelesen. Ausgabe: erste fehlende
Referenzaktion nach `gsp 0x001E`, Kontext und die Liste fehlender
Ereignisse. Wiederholungen werden standardmaessig zusammengefasst.

## 10. matrix — A/B-Runner

```powershell
python tools\matrix.py --matrix "HLE_RM_ADV=0|1" --timeout 15 --jobs 2
python tools\matrix.py --matrix "HLE_TICK_MS=2|8" --repeat 2 --deterministic
```

Startet jede Kombination, wertet `milestones` aus, zeigt `M1..M9` als
`+`/`.` und nennt den besten Lauf. `--repeat` prueft die Determinismus-
Signatur. `--deterministic` setzt `HLE_DET_SCHED=1` und `HLE_TICK_MS=2`.

## 11. milestones — Fortschrittskette

```powershell
python tools\milestones.py --log out\boot_log.txt
python tools\milestones.py --run --timeout 20
```

Kette: APT -> FS/RomFS -> DSP -> GSP-Init -> boss:U -> SaveData/head.lt6 ->
`gsp 0x000C` -> VRAM-Writes -> echter Frame. Jede Zeile mit `✅`/`❌` und
Beweiszeile; Exit-Code 1, wenn eine Stufe fehlt.

## 12. hotpatch — Hooks ohne Rebuild

Runtime: `HLE_HOOK=ADRESSE=CALLBACK[,ADRESSE=CALLBACK]`. Verfuegbar sind
`count_fields`/`count`, `regs`/`dump_regs`, `mem`/`dump_mem`, `log`/`pc`
und `set_r0` (mit `HLE_HOOK_VALUE`).

```powershell
python tools\hotpatch.py list
python tools\hotpatch.py run --hook 134818=count --timeout 5
python tools\hotpatch.py run --hook 28F284=regs,28F284=mem --timeout 5
```

## 13. perfmap — Block-Histogramm und Rate

```powershell
python tools\perfmap.py run --timeout 12 --top 8
python tools\perfmap.py report --perfmap tools\out\perfmap.txt --seconds 3.0
```

Aggregiert Bloecke pro Funktion/Thread, zeigt Top-Rueckwaertskanten und
schaetzt emulierte Sekunden pro Wandsekunde (268,111 MHz, Blocklaenge
statisch bis zum naechsten Sprung).

## 14. framecheck — Screenshot vs. Referenz

```powershell
python tools\framecheck.py
python tools\framecheck.py --fail-on-placeholder --json-out tools\out\frame.json
```

Liest BMP und PNG mit der Standardbibliothek, vergleicht 96x54-Downscales,
zeigt dominante Farben/Perzentile und meldet den Port-Platzhalter
(288.054 Byte = 400x240 ohne Spiel-Render).

## 15. Fuer neue Agenten

Skill lesen: `.opencode/skills/3ds-hle/SKILL.md` fuer IPC/HLE-Details und
`.opencode/skills/3ds-port/SKILL.md` bzw. `docs/SKILL.md` fuer die
Messrezepte. Der 3ds-hle-Skill enthaelt die
IPC-/Reply-Fallen, die verifizierten Dienst-Cmd-IDs, das RomFS/IVFC-Layout,
GSP-Shared-Memory-Details, libctru-Referenzen und die bekannten Sackgassen.
Die Werkzeuge sind dort mit ihren Aufrufen verlinkt; damit findet ein neuer
Agent ohne Vorwissen zuerst `guest-refs`/`trace-diff` und dann die richtige
Messung.

## 16. Hilfsskripte (Projekt-Wurzel, nicht `g3ds`)

| Skript | Zweck |
|---|---|
| `tools\_make_menu_frame.py` | erzeugt `out/menu_official.bin` (Top 400x240) und `reports/menu_official_bottom.bin` (Bottom 320x240) aus den Azahar-Captures |
| `tools\_cmp_shot.py` | Pixeldiff BMP/PNG gegen `reports/menu_official*.bin` (`diff_pixels`, `maxdiff`) |
| `tools\_analyze_screens.py` | Capture-Geometrie (aktiver Screen, Bounding-Box, Grob-Raster) |
| `tools\_tidy_out.ps1` | raeumt `out/` in `logs/`, `shots/`, `probes/`, `pdb/`, `artifacts/`, `backup/` auf |

## 17. `out/`-Ablage (aufgeraeumt)

```
out/port.exe, out/boot.exe      aktuelle Binaries
out/romfs/                      extrahierte RomFS (3 .fa, 566 BCSTM, 34 MOFLEX)
out/exefs/code_decomp.bin       dekompilierter Code (Basis 0x100000)
out/romfs_image.bin             gepatchtes SelfNCCH-Image (pristine)
out/menu_official.bin           Top-Referenz (400x240 RGB)
out/menu_official_bottom.bin    Bottom-Referenz (320x240 RGB)
out/shot.bmp                    aktueller GPU-Screenshot
out/logs/                       Mess-Logs (Historie)
out/shots/                      weitere Screenshots
out/probes/                     Probe-/Alt-Binaries
out/pdb/                        Debug-Symbole
out/artifacts/                  Traces, .o, .raw, missing_calls
out/backup/                     romfs_image.hashpatch.bak
```

`out/` und `build/` sind nicht versioniert; die Referenzframes unter
`reports/` schon.
