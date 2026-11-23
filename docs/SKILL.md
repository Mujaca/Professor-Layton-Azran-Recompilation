# SKILL: Messen, lokalisieren, vergleichen (3DS-Static-Port)

Dieses Dokument ist der Einstieg fuer neue Agenten. Es beschreibt die
**exakten Kommandos** und die **gemessenen Fallstricke** des Projekts.
Kurzpfad: `messen -> lokalisieren -> Referenz vergleichen -> A/B-Test`.

Repo: `C:\Users\user\Documents\Repos\3DS` (alle Pfade relativ dazu).

## 1. Build und Selbsttests

```powershell
# Port (Fenster) neu bauen
python tools\build_port.py                  # -> out\port.exe

# Nur Runtime neu bauen, ohne gen_boot.c zu regenerieren (schnell)
python tools\build_boot.py                  # -> out\boot.exe (headless)

# Headless-Boot (ohne Fenster); beendet sich nach ~10 s selbst
.\out\boot.exe out\romfs

# Referenzframes (Top 400x240 + Bottom 320x240) erzeugen - nur Vergleich
python tools\_make_menu_frame.py

# Kanonischer Engine-PICA-Lauf (Strom @0x30 -> 0x0CFE6000)
python tools\run_chain_direct.py --profile tools\profiles\engine_pica_draw.json --shot out\shot_tmp.bmp
python tools\canon.py --log out\logs\chain_direct.txt --shot out\shot_tmp.bmp --engine
python tools\measure_ref.py out\shot_engine.bmp

# Pflicht-Selbsttests
python tests\mvp_selftest.py                # 13/13
python tools\selftest_tools.py              # 16/16
```

Aktueller Menue-Stand (Commit `854975c`, verifiziert): TOP sichtbar
`69443/90400` mean `48.32`, BOTTOM sichtbar `14752/71360` mean `14.33`
(`HLE_PICA_CLEAR=1`, deterministischer Shot). Der Draw-Block stammt aus dem Engine-Strom @`0x30` ->
`0x0CFE6000`; Quads/Vertex-Attribute und Texturen emittiert jedoch noch HLE
(`HLE_PICA_SPRITES`). Der fruehere „0 Pixel Diff“ galt nur fuer die
Referenz-Capture-Kopie, nicht fuer Spiel-Assets. Die Scene-Paar-Kette
`11A3E0 -> 27C2BC -> 2794A4 -> 2721E0` laeuft (Profil
`tools/profiles/scene_pairs.json`). Adresskorrektur: die Paare liegen bei
`ctx+0xB34/0xB38` und `ctx+0xB3C/0xB40` (nicht `0x8334`/`0x833C`) und sind
**PICA-Paare**, keine Float-UVs; die UVs stehen im Desc-Buffer
`[ctx+0xD6C]`. Offen sind der 0xD2-Container-Link und der natuerliche
Submit.

`python tools\_cmp_shot.py` vergleicht einen einzelnen 400x240-Frame; fuer
den 400x480-Portshot ist `measure_ref.py` die kanonische Messung (sichtbare
226/223 Zeilen). `PORT_NO_OFFICIAL=1` deaktiviert den Referenz-Loader;
`PORT_NO_MENU_GPU=1` schaltet auf den alten Host-Frame-Fallback.

`python tools\build_port.py` regeneriert `build/gen_boot.c` (dauert) und
wendet die SDK-Patches aus `tests/port.c` an. `build_boot.py` nutzt die
vorhandene `gen_boot.c` und ist der richtige Weg fuer
Instrumentierungs-Iterationen.

## 2. Die fuenf wichtigsten Kommandos

```powershell
# 1) Wo wartet der Port? (Perfmap + Felder + Schreiber, < 60 s)
python tools\where.py --run --thread 2

# 2) Statische Fragen (einmal indexieren, dann Millisekunden)
python tools\callgraph.py writers 0x40B4C4
python tools\callgraph.py callers 0x28F250
python tools\callgraph.py literals 0x134818
python tools\callgraph.py strings "boss:U"

# 3) Referenz-Diff ab gsp 0x001E
python tools\diff_trace.py --port tools\fixtures\port_boot_20261119.log

# 4) Fortschrittskette mit Beweiszeilen
python tools\milestones.py --log tools\fixtures\port_boot_20261119.log

# 5) Screenshot gegen Azahar-Referenz
python tools\framecheck.py
```

Alle Werkzeuge haben `--help`, schreiben nur nach `tools/out/` und sind
rein lesend bzw. env-gated.

## 3. Messlauf von Null (Rezept)

```powershell
# Messlauf starten, Log liegt unter tools\out\runs\<zeit>_<label>_<pid>\
python tools\guest_run.py --timeout 20 --until "srvname.*boss:U" --label hypo1

# Ergebnis bewerten (ohne --log wird der neueste Lauf genommen)
python tools\milestones.py
python tools\diff_trace.py --port tools\out\runs\<...>\hypo1.out.log
```

`guest_run.py` setzt `HLE_SRV_NAMES=1` und `HLE_PERF=1`; stdout/stderr
werden getrennt mitgeschnitten. Ein Timeout ist kein Fehler, sondern ein
gueltiges Messergebnis (Exit-Code 0).

## 4. Werkzeugkasten

| Werkzeug | Zweck |
|---|---|
| `tools\where.py --run` | heisse Funktion, Spin-Kante, gelesene Felder, Schreiber (statisch + dynamisch) |
| `tools\callgraph.py` | Programmdatenbank: `callers`, `literals`, `writers`, `readers`, `strings`, `fn`, `stats` |
| `tools\diff_trace.py` | Referenz vs. Port ab Anker; erste fehlende Aktion + fehlende Ereignisse |
| `tools\matrix.py` | A/B-Matrix ueber `HLE_*`-Schalter, Meilensteine, bester Lauf |
| `tools\milestones.py` | Kette APT -> FS -> DSP -> GSP -> boss:U -> SaveData -> 0x000C -> VRAM -> Frame |
| `tools\hotpatch.py` | `HLE_HOOK=<adresse>=<callback>` ohne Rebuild |
| `tools\perfmap.py` | Block-Histogramm pro Funktion/Thread, Emulationsrate |
| `tools\framecheck.py` | BMP/PNG-Vergleich, dominante Farben, Platzhalter-Erkennung |

Beispiele:

```powershell
# Block-Histogramm und Rate; stoppt nach dem ersten Dump
python tools\perfmap.py run --timeout 12 --top 8

# Hotpatch: Tick-Task zaehlen, ohne Rebuild
python tools\hotpatch.py run --hook 134818=count --timeout 5

# Hotpatch: an einer Adresse Register und Speicher zeigen
python tools\hotpatch.py run --hook 28F284=regs,28F284=mem --timeout 5

# A/B: HLE_RM_ADV an/aus, Meilenstein-Tabelle
python tools\matrix.py --matrix "HLE_RM_ADV=0|1" --timeout 15 --jobs 2
```

## 5. Fallstricke-Katalog (gemessen)

### `getenv()` pro Block
`arm_trace_block` und `arm_mem_read` haben frueher pro Block bzw. pro
Speicherzugriff `getenv()` gerufen. Das war der Hauptbremsklotz. Env-Flags
werden jetzt einmalig in `TraceEnv` gecacht. Neue Schalter dort ergaenzen,
nicht im Hot Path `getenv()` einfuegen.

### `GetTickCount64()` hat ~15,6 ms Aufloesung
Fuer kurze Messungen nicht als Uhr verwenden. `HLE_TICK_MS` und
`HLE_PERFMAP_DUMP_MS` sind entsprechend grob; die Blockrate aus `HLE_PERF`
ist aussagekraeftiger als Wandzeit.

### IPC-Reply-Layout (Off-by-one)
Der Header liegt in `cmdbuf[0]`. `ipc_put` beginnt bei `buf[0]`; deshalb
muss jede Reply mit `Result + N` zuerst ein Dummy-Wort pushen:
`Dummy, Result, Parameter...`. Sonst rutscht alles um ein Wort.
Beleg: `runtime/hle/hle.c` `ipc_finish`/`put_result`; Skill `3ds-hle`.

### Alpha-Blending und Frame-Akkumulation
`put_px_blend` mischt Quell-Alpha ueber das Ziel (`out = src*a + dst*(1-a)`),
ohne implizites Frame-Clear. Ohne `HLE_PICA_CLEAR=1` akkumuliert der
Shot ueber die Frame-Anzahl (gemessen: TOP `65707` vs `85271`). Fuer
Bildmessungen daher immer `HLE_PICA_CLEAR=1` setzen und 2x wiederholen.
`HLE_PRESENT_FILL=0` schaltet den `#604020`-Platzhalter ab.

### PICA-Register
Kurzmap (`0x200` Basis, `0x201/0x202` Formate, Loader `0x203 + l*3` mit
`byte_count` in Wort 2 Bits 16..23, `0x228` nv, `0x22E/0x22F` Draw,
`0x80` Texture-Enable, `0x8E` Format) steht im Skill `3ds-hle`.

### `HLE_NO_THREAD_LOCK`
Schaltet den Gast-Lock ab (`guest_lock_enter/leave`). Ohne Lock koennen
Threads parallel Speicher lesen/schreiben und Messungen werden nicht
deterministisch. Fuer A/B-Matrizen Standard (Lock an) beibehalten;
`HLE_DET_SCHED=1` erzwingt zusaetzlich Round-Robin.

### `HLE_APT_STATE`
Erzwingt einen APT-Zustand. Nur fuer Diagnose; veraendert den
Gastzustand und damit die Dienstsequenz. Immer mit `milestones.py` und
`diff_trace.py` gegen den Normallauf vergleichen.

### `HLE_TICK`
`HLE_TICK=0` schaltet den VBlank-/Tick-Dispatcher ab. Dann bleiben
`0x40B4C4`/`0x40B4C8` auf 0 und `fn_0028F250` wartet endlos. Das ist
**kein Deadlock des Ports**, sondern ein fehlender externer Consumer.
Standard: an, `HLE_TICK_MS` Default 2.

### RomFS/IVFC-Groessen
`out/romfs_image.bin` ist ein SelfNCCH-RomFS. Die IVFC-Groessen stehen als
Little-Endian-`u32` bei `0x10`, `0x18`, `0x20`; der Metadaten-Header
beginnt bei `0x1000`, `file_data_offset` bei `0x1024`. Der Port oeffnet
SelfNCCH ueber `OpenFileDirectly` (Cmd `0x0803`, Archive-ID 3). Falsche
Groessen fuehren zu Hash-Tabellen als Nutzdaten.

### SelfNCCH = 3
Archive-ID 3 ist die SelfNCCH/Programm-RomFS. `fs:USER 0x0803` mit einer
anderen ID liefert `Result` ungleich 0 und die Asset-Registrierung bricht
ab. Siehe `tools/romfs_image.py` und `tools/g3ds/refs.py`.

### `ARM_MEMWATCH`-Format
Bei Adressen >= `0x10000000` schreibt der Memwatch `mw] 1F...` (nicht
`mw] 001F...`). `tools/g3ds/watch.py` parst beide Formen.

### Literal-Pools vs. Funktiongrenzen
`build/gen_boot.c` enthaelt neben echten Funktionen auch Seed-Labels
mitten in Funktionen. `genmap.FunctionMap` liefert dort zu kurze
Funktionen. Fuer lokale Analysen grosszuegig bis `start+0x200/0x400`
dekodieren (macht `tools/where.py`).

## 6. Fortschrittskette (Single Source of Truth)

`tools/milestones.py` prueft in dieser Reihenfolge:

1. APT-Parameter (`ReceiveParameter` / `0x000D` / `0x000E`)
2. FS/RomFS geoeffnet (`[fs]`, `OpenFileDirectly`, `file 0x0802/0x0803`)
3. DSP initialisiert (`dsp::DSP`, `[dsp]`)
4. GSP-Init (`0x0013` + `0x001E`)
5. `boss:U` akquiriert
6. SaveData / `head.lt6` geoeffnet
7. `gsp::Gpu 0x000C` (TriggerCmdReqQueue)
8. VRAM-Writes (`[mw] 1F...`)
9. echter Frame (`SetBufferSwap` + VRAM-Write)

Jede Zeile enthaelt `✅`/`❌` und die konkrete Beweiszeile. Kein
Statusbericht ohne diesen Checker.

## 7. Bekannter Meilenstein-Stand (Commit `854975c`, 2026-11-23)

- Kanonisch: Engine-Strom @`0x30` -> `0x0CFE6000`; TOP sichtbar
  `69443/90400` mean `48.32`, BOTTOM `14752/71360` mean `14.33`
  (`HLE_PICA_CLEAR=1`, deterministischer Shot, `canonical_shot.json`).
- Attribute/Texturen kommen aus `HLE_PICA_SPRITES` (Spiel-Assets
  `title_a.xa` + `title_b_bg`), **nicht** aus `reports/menu_official*.bin`.
  „0 Diff“ ist nur noch der Referenz-Capture-Fall.
- **Offen:** 0xD2-Container-Link und natuerlicher Engine-Submit ohne
  `HLE_FRAME_LOOP2`. Die Scene-Paar-Kette laeuft
  (`11A3E0`/`27C2BC`/`2794A4`/`2721E0` mit `scene_pairs.json`); die Paare
  liegen bei `ctx+0xB34/0xB3C` und sind **PICA-Paare**, keine Float-UVs
  (UVs im Desc-Buffer `[ctx+0xD6C]`). Details:
  [`PORT_STATE.md`](PORT_STATE.md).

Statisch belegter RM-Wartepunkt (weiterhin gueltig):

```text
Thread 2 wartet in fn_0028F250 auf Feld 0x0040B4C4;
letzter Schreiber: fn_00134818 (Tick-Task Slot 2 der Tabelle 0x003D7C50)
```

## 8. Schnelle Einmal-Fragen (ohne neues Skript)

```powershell
python tools\callgraph.py fn 0x28F250
python tools\callgraph.py writers 0x40B4C4
python tools\callgraph.py readers 0x40B4C4
python tools\callgraph.py strings "boss:U"
python tools\callgraph.py literals 0x134818
```

Der Index wird automatisch aus `code_decomp.bin` + `gen_boot.c` gebaut
(`tools/out/callgraph.json`) und bei Aenderungen invalidiert. Mit
`--rebuild` erzwingen.

## 9. Commit-Stil

Kleine Commits, im Text **was gemessen wurde** (Adresse, Logzeile,
Ergebnis), nicht nur was geaendert wurde. Beispiel:

```text
Hotpatch-Hooks: HLE_HOOK=134818=count zaehlt den Tick-Task ohne Rebuild;
Messung: 190 Treffer in 8 s, letzter Schreiber pc=0x00134818.
```
