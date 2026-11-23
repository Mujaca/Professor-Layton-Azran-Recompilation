# HANDOVER — 3DS-Port (Stand 2026-11-23, 17:30)

Dies ist die einzige Einstiegsdatei. Danach `docs/PORT_STATE.md` (letzte
Einträge) und `docs/SKILL.md` lesen. **Skills `3ds-hle` und `3ds-port`
laden, bevor der erste Befehl läuft.**

## 1. Ziel

Das echte Hauptmenü soll von der Spiel-Engine gebaut und über `gsp::Gpu`
(`SubmitCmdList`/`SetCommandList`, `TriggerCmdReqQueue`,
`SetBufferSwap screen=0/1`) in beide Framebuffer gerendert werden:
oben 400x240, unten 320x240, in einem 400x480-Fenster — ohne Host-`memcpy`,
ohne Referenz-Blit und ohne HLE-0x070FFFFF-Assetliste. Die Pixel müssen aus
einer engine-emittierten Liste bei `0x0CFE6000` stammen.

## 2. Verifizierter Stand (jede Zeile hat einen Beleg)

| Fakt | Beleg |
|---|---|
| Engine-Submit `0x12FD30 -> 0x139914 -> 0x13F2D4` läuft **ohne HLE_RETFORCE** (direkter Start) | `out/logs/run.txt`, Profil `tools/profiles/engine_pica_30.json`; Hooks 19–26 Hits |
| Engine schreibt ihre Knotenliste selbst nach `0x0CFE6000` | Dump `0CFE6000: 00000000 0CFE6004 0CFE6004 …`; `[gspq-engine] … ops=1` |
| `qops=1` aus der Engine-Liste (gsp_engine_list/PICA), nicht aus HLE-Deskriptor | `[gspq-engine] list=0CFE6000 ops=1`, `Trigger lr=0012FE1C … qops=1` |
| HLE-0x070FFFFF-Liste und Asset-`memcpy` sind abschaltbar | `HLE_2D_NO_BLIT=1`; kein `Menue-Assets geladen`; `vs menu_top.bin 95368/96000` |
| **Echter PICA-Draw-Strom der Engine liegt @`0x00000030`** (inkl. `0x22F`) | Dump in `out/logs/engine.txt`; Submitting `0x30/0x80` → `ops=1` |
| Erzeuger: `0x1323FC` (Template `0xF0228`/`0xF022F`), aufgerufen über `0x27B34C`-Bind | `out/logs/engine.txt`; Hooks `1323FC=2`, `132610=2` |
| Bind-Gates: `[ctx+0xB4C]=0x409354` und `[desc+0x20]=0x0CFE7090` | `tools/profiles/sprite_bind_gates.json` |
| CFile-Pool `0x40F048` wird vom echten Engine-Loop `0x11ACBC` mit 16 CFiles (Vtable `0x3C9B18`) gefüllt | `out/logs/run.txt`; OBJADDR-Dump |
| Menü-`.xq` ohne erzwungenen Open lesbar (Pool-Ring + echter ARC0-Index) | `tools/profiles/menu_xq_noforce.json`; `[faread] idx=0 pos=00458CB8 n=72` |

Aktuelle Pixel (Engine-PICA-Lauf, noch **nicht** das Menü):

```
python tools\measure_ref.py out\shot_engine.bmp
TOP 89358/90400, BOTTOM 71353/71360   (flach 96,64,32)
```

Letzter Commit-Stand: `c49a386` (danach kamen nur Handoff/Profile).

## 3. Was noch erzwungen wird (explizit)

1. `HLE_FRAME_LOOP2=12FD30` statt des natürlichen `0x12493C`-Pfads; der
   bricht ohne Force an `0x26F130` ab. Descriptor-/C-Tabellen-POKEs
   (`0x0EA00000…`, count=1) sind synthetisch.
2. Ring-Strides `0x40F04C/0x40F058/0x40F064 = 4` (Statik-Ctor fehlt).
3. Bind-Gates `0x409354`, `0x0CFE7090` und `HLE_PRECALL=27B34C,…`.
4. `HLE_2D_NO_BLIT` (HLE-Liste/Assets aus).
5. `rom:`-Registrierung offen: `0x408150` leer; `0x26E95C` erwartet
   UTF-16 `L"rom:/…"`; Registrar ist `0x287BB4` (nicht `0x26E7C0`!), und
   der Eintrag braucht ein echtes ROMFS-Objekt. Der authentische Mount
   `0x100B4C` hängt im Port.
6. Init-Kette `0x289B9C/0x11A890/0x11B05C/0x2919DC` wird HLE-getrieben.

## 4. Nächste Schritte (in dieser Reihenfolge)

**A. Engine-PICA-Draw sichtbar machen (höchster Hebel).**
Der Strom @`0x30` ist gültig und wird mit `ops=1` ausgeführt, malt aber
nichts. In `runtime/gpu.c` den `0x22F`-Draw (DrawArrays) mit Vertex-Arrays
(`0x200…0x24x`) und Textur (`0x1E000000`, 400x240 RGB) vervollständigen.
Ziel: `out/shot_engine.bmp` zeigt Menü-Pixel; danach
`python tools\measure_ref.py out\shot_engine.bmp`.

**B. Engine-Stream natürlich an `0x0CFE6000` bringen.**
Statt Ring-Poke (`10002800/…`) den Engine-Submit/Trigger nutzen
(`0x12FD30` -> `0x13E3E0`), prüfen `[gspq-engine] list=0CFE6000`.

**C. Forcing abbauen.**
`rom:`-Registrierung (0x287BB4 mit echtem ROMFS-Objekt oder 0x100B4C),
Ring-Strides über echten Ctor, `0x12493C`-Gates (`0x26F130`), dann
`HLE_FRAME_LOOP2` und Descriptor-POKEs entfernen.

**D. HLE-Assetpfad komplett löschen** (nur noch Engine), Top/Bottom messen.

## 5. Arbeitsweise — bitte tiefer und disziplinierter als bisher

### 5.1 Skills zuerst

`3ds-hle` und `3ds-port` laden. Rezepte aus `docs/SKILL.md` aktiv nutzen:
`hotpatch.py`, `framecheck.py`, `trace-diff`, `guest-refs`,
`guest-watch`, `HLE_BLKLOGF`, `obj`-Hook. **Rebuild nur**, wenn Scanner,
Emitter oder Runtime-C geändert wurde; sonst Env/Hotpatch.

### 5.2 Sub-Agents parallel (Pflicht pro Runde)

Zu Beginn jeder Runde **2–3 Sub-Agents** (`explore` oder `general`)
parallel starten, je ein klar abgegrenzter Track:

- **(a) Statische Analyse:** Disassembly einer Adresse/Funktion, Gates,
  Aufrufer, Vtable-Slots. Ergebnis: Adressen + Bedingungen.
- **(b) Log-/Referenz-Forensik:** `out/logs/run.txt`, `reports/azahar_trace.txt`,
  `tools/fixtures/*`, `trace-diff`; sucht die erste echte Divergenz.
- **(c) Code-Pfad-Suche:** welcher Writer/Handler emittiert was, welche
  Datei/Zeile müsste geändert werden (ohne Änderung).

Regeln: Jeder Prompt enthält Repo-Pfad, bekannte Adressen, exakte Frage,
erwartetes Artefakt (Adressliste/Datei:Zeile/Kommando). Während die Agenten
laufen, genau **ein** eigener Messlauf. Keinen Track doppelt bearbeiten.
Agenten-Ergebnisse sofort in `docs/PORT_STATE.md` festhalten.

### 5.3 Anti-Spin-Regeln

- Pro Runde **ein** neuer belegter Fakt + Commit + Tests + Handoff-Update.
- Vor jedem Experiment prüfen: steht es in „Sackgassen" (unten) oder ist
  es schon belegt?
- Jede Behauptung braucht Logzeile, Dump oder Dateiinhalt; keine
  „sollte"-Aussagen.
- Messungen immer gegen `reports/menu_official*.bin` mit
  `tools\measure_ref.py` und die Zahlen in `docs/PORT_STATE.md` schreiben.

## 6. Kanonische Dateien (feste Namen, kein Spam)

| Zweck | Datei |
|---|---|
| Screenshot Kettenlauf | `out/shot.bmp` |
| Screenshot Engine-Lauf | `out/shot_engine.bmp` |
| Log Kettenlauf | `out/logs/run.txt` |
| Log Engine-Lauf | `out/logs/engine.txt` |

Nach jedem Lauf:

```
python tools\run_chain_direct.py --profile tools\profiles\<profil>.json --shot out\shot_tmp.bmp
python tools\canon.py --log out\logs\chain_direct.txt --shot out\shot_tmp.bmp [--engine]
python tools\canon.py --clean
```

`tools/canon.py --clean` löscht alle anderen bmp/png/logs in `out`.
Assets/RomFS bleiben unberührt.

## 7. Wichtige Dateien / Assets

- `out/romfs_image.bin` — **pristine**, nie überschreiben.
- `out/romfs/` — Spieldaten (read-only), `out/exefs/code_decomp.bin`.
- `out/fa_a/seq/menu_logo.xq` — Byte-Referenz für den ARC0-Index.
- `out/menu_top.bin`, `out/menu_bottom.bin` — nur HLE-Fallback.
- `reports/menu_official.bin`, `reports/menu_official_bottom.bin` —
  Vergleichsreferenz, nicht ändern.
- `out/port.exe` — kanonisches Binary; Build-Varianten wurden gelöscht.
- Profile: `tools/profiles/chain.json` (Kette), `engine_pica_30.json`
  (Engine-PICA @0x30), `sprite_bind_gates.json`, `menu_xq_noforce.json`,
  `real_entry.json`.

## 8. Sackgassen / Fallstricke (aus dieser Session)

- `0x26E7C0` ist **Unregister** (schreibt Nullen); Registrar ist `0x287BB4`.
- `0x26E95C` hasht UTF-16 bis `:` in max. 8 Units; ASCII/ohne `:` → 0.
- `0x3C4F4C + 0x78 = 0` → `0x1BD720` allein kann nicht zeichnen;
  `0x297B24` läuft, aber kein GX-Writer-Treffer.
- `0x100022AC` ist **nicht** der 2D-GX-Kontext; `0x408808` hat `[+4]=0`.
- `0x140784`/`0x143F84` haben 0 statische Aufrufer, im 2D-Lauf 0 Treffer.
- `0x0CFE7200` enthält nur Textur-Setup; der echte PICA-Strom @`0x30`.
- `0x11ACBC`-Loop läuft nur mit Ring-Strides; sonst `0x284A48` → 0.
- `0x0CFE6000` ist in der Legacy-Whitelist; PICA-Fallback ist implementiert.

## 9. Kanonische Befehle

```powershell
# Skills laden (im Agenten-Tool), dann:
python tools\build_port.py
python tests\mvp_selftest.py
python tools\selftest_tools.py

# Kette
python tools\run_chain_direct.py --profile tools\profiles\chain.json --shot out\shot_tmp.bmp
python tools\canon.py --log out\logs\chain_direct.txt --shot out\shot_tmp.bmp --clean

# Engine-PICA-Draw @0x30
python tools\run_chain_direct.py --profile tools\profiles\engine_pica_30.json --shot out\shot_tmp.bmp
python tools\canon.py --log out\logs\chain_direct.txt --shot out\shot_tmp.bmp --engine --clean

# Messung
python tools\measure_ref.py out\shot_engine.bmp
```

## 10. Offene Fragen für die Sub-Agents

1. Wer setzt den 2D-GX-Kontext (C-Tabelle) — und warum ist `[0x408808+4]=0`?
2. Wie malt der echte PICA-Pfad den `0x22F`-Draw (Vertex-Arrays + Textur)?
3. Wie registriert man `rom:` ohne Hänger (`0x100B4C`)?
4. Welche Vtable mit gültigem `+0x78` ist die echte Sprite-Draw-Klasse?
5. Wie kommt der Strom @`0x30` ohne Ring-Poke an `0x0CFE6000`?
