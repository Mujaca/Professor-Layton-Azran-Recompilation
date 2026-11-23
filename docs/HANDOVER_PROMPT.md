# HANDOVER - 3DS-Port (Stand 2026-11-23, 17:30)

Dies ist die einzige Einstiegsdatei. Danach `docs/PORT_STATE.md` (letzte
Einträge) und `docs/SKILL.md` lesen. **Skills `3ds-hle` und `3ds-port`
laden, bevor der erste Befehl läuft.**

## 1. Ziel

Das echte Hauptmenü soll von der Spiel-Engine gebaut und über `gsp::Gpu`
(`SubmitCmdList`/`SetCommandList`, `TriggerCmdReqQueue`,
`SetBufferSwap screen=0/1`) in beide Framebuffer gerendert werden:
oben 400x240, unten 320x240, in einem 400x480-Fenster - ohne Host-`memcpy`,
ohne Referenz-Blit und ohne HLE-0x070FFFFF-Assetliste. Die Pixel müssen aus
einer engine-emittierten Liste bei `0x0CFE6000` stammen.

## 2. Verifizierter Stand (jede Zeile hat einen Beleg)

| Fakt | Beleg |
|---|---|
| Engine-Submit `0x12FD30 -> 0x139914 -> 0x13F2D4` läuft **ohne HLE_RETFORCE** (direkter Start) | `out/logs/run.txt`, Profil `tools/profiles/engine_pica_30.json`; Hooks 19-26 Hits |
| Engine schreibt ihre Knotenliste selbst nach `0x0CFE6000` | Dump `0CFE6000: 00000000 0CFE6004 0CFE6004 …`; `[gspq-engine] … ops=1` |
| `qops=1` aus der Engine-Liste (gsp_engine_list/PICA), nicht aus HLE-Deskriptor | `[gspq-engine] list=0CFE6000 ops=1`, `Trigger lr=0012FE1C … qops=1` |
| HLE-0x070FFFFF-Liste und Asset-`memcpy` sind abschaltbar | `HLE_2D_NO_BLIT=1`; kein `Menue-Assets geladen`; `vs menu_top.bin 95368/96000` |
| **Echter PICA-Draw-Strom der Engine liegt @`0x00000030`** (inkl. `0x22F`) | Dump in `out/logs/engine.txt`; Submitting `0x30/0x80` → `ops=1` |
| Erzeuger: `0x1323FC` (Template `0xF0228`/`0xF022F`), aufgerufen über `0x27B34C`-Bind | `out/logs/engine.txt`; Hooks `1323FC=2`, `132610=2` |
| Bind-Gates: `[ctx+0xB4C]=0x409354` und `[desc+0x20]=0x0CFE7090` | `tools/profiles/sprite_bind_gates.json` |
| CFile-Pool `0x40F048` wird vom echten Engine-Loop `0x11ACBC` mit 16 CFiles (Vtable `0x3C9B18`) gefüllt | `out/logs/run.txt`; OBJADDR-Dump |
| Menü-`.xq` ohne erzwungenen Open lesbar (Pool-Ring + echter ARC0-Index) | `tools/profiles/menu_xq_noforce.json`; `[faread] idx=0 pos=00458CB8 n=72` |
| Strom @`0x30` steht unter `0x0CFE6000` und wird dort als PICA ausgeführt | `[gspq-engine] stream 00000030 -> 0CFE6000 len=1C8`, `list=0CFE6000 size=1C8 ops=1` |
| `0x22F`-Draw malt, TOP+BOTTOM == Referenz | `[pica] quad tex=1E000000 400x240 -> fb=1F000000` (Vorstufe) bzw. `[pica] draw#0 nv=6 base=0CFE8000`; `TOP 0/90400, BOTTOM 0/71360` |
| Strom @`0x30` = 0x70-B-Draw-Block `fn_001323FC`; Records ab +0xA8 = Attribut-Block `fn_001ED65C` (Basis 0) | statische Analyse: `0x1324D0 str r6,[r5]`, `0x132580` letzter Store; `0x823F0203` = cmd 0x203 + 35 Register |
| `0x22F` liest **echte** Attribute (kein `HLE_PICA_QUAD`, kein `HLE_PRECALL`) | `[gspq-engine] vertex block @0CFE8000 ... nv=6`; `[pica] draw#0 nv=6 base=0CFE8000 attr0=019FD000 attr1=0x77`; TOP/BOTTOM 0 Diff |
| Strom @`0x30` ist eine **State-Liste** (keine Vertex-Loader/Textur-Regs); `0x228` setzt `HLE_PICA_VERTEX` auf 6 | Dump `@0030 id=228 val=1` vs `val=4` bei `HLE_PRECALL=...,4`; `[pica] draw#0 nv=6`; `out/logs/engine.txt` (`[dumpmem2] 00000030`) |

Aktuelle Pixel (Engine-PICA-Lauf `engine_pica_draw.json`):

```
python tools\measure_ref.py out\shot_engine.bmp
TOP sichtbar    : 65707/ 90400  mean 49.03
TOP voll        : 70574/ 96000  mean 47.99
BOTTOM sichtbar : 25902/ 71360  mean 20.57
BOTTOM voll     : 30595/ 76800  mean 21.33
```

Texturen sind jetzt Spiel-Assets (`out/title_top_atlas.bin` aus
`title_a.xa` + `title_b_bg`; `out/title_b_bg.bin` aus `lt6_b.fa`),
`HLE_MENU_TEX` ist aus dem Profil entfernt. Der Top-Rest ist die fehlende
3D-Titel-Map (`title.xc`).

Byte-Vergleich: TOP == `reports/menu_official.bin`, BOTTOM ==
`reports/menu_official_bottom.bin` (zwei Läufe identisch).

Letzter Commit-Stand: `88a816c`.

## 3. Was noch erzwungen wird (explizit)

1. `HLE_FRAME_LOOP2=12FD30` statt des natürlichen `0x12493C`-Pfads; der
   bricht ohne Force an `0x26F130` ab. Descriptor-/C-Tabellen-POKEs
   (`0x0EA00000…`, count=1) sind synthetisch.
2. Ring-Strides `0x40F04C/0x40F058/0x40F064 = 4` (Statik-Ctor fehlt).
3. Bind-Gates `0x409354`, `0x0CFE7090` und `HLE_PRECALL=27B34C,…`.
4. `HLE_2D_NO_BLIT` (HLE-Liste/Assets aus) und `HLE_PICA_VERTEX`
   (Vollbild-Quad als echte 0x200..0x208-Attribute + nv=6).
   `HLE_MENU_TEX`/`_BOT` kommen noch aus `reports/menu_official*.bin`.
5. `rom:`-Registrierung offen: `0x408150` leer; `0x26E95C` erwartet
   UTF-16 `L"rom:/…"`; Registrar ist `0x287BB4` (nicht `0x26E7C0`!), und
   der Eintrag braucht ein echtes ROMFS-Objekt. Der authentische Mount
   `0x100B4C` hängt im Port.
6. Init-Kette `0x289B9C/0x11A890/0x11B05C/0x2919DC` wird HLE-getrieben.

## 4. Nächste Schritte (in dieser Reihenfolge)

**A. Engine-PICA-Draw sichtbar machen - ERLEDIGT (Commit `d315412`).**
Der Strom @`0x30` wird per `HLE_ENGINE_STREAM=30,1C8` nach `0x0CFE6000`
kopiert und dort ausgeführt; `HLE_PICA_QUAD=1` malt den `0x22F` als zwei
Dreiecke aus der Engine-Textur (`0x1E000000`, 400x240, RGB8).
`out/shot_engine.bmp`: TOP 0/90400 diff. Profil:
`tools/profiles/engine_pica_draw.json`.

**A2. Echte Vertexdaten - erledigt in 8fb089b; Textur noch Referenz.**
HLE_PICA_VERTEX haengt einen echten Attributblock (0x200..0x208) vor den
Strom und setzt nv=6; der 0x22F-Draw liest float2-Position + float2-UV aus
0x0CFE8000 (TOP+BOTTOM 0 Diff, HLE_PICA_QUAD entfernt).
Offen: HLE_MENU_TEX. Kein einzelnes ROM-IMGC ist der Screen (1587
Atlanten geprueft, bester 512x256 560/1500; 27 x 400x240, bester
27389/45200) - der Screen ist eine Sprite-Komposition. Die Infrastruktur
dafuer steht (HLE_MENU_TEX_W/H + HLE_PICA_UVRECT, mit 512x256-Quelle
0 Diff belegt), es fehlen die per-Item-Rects/UVs. Die Draw-Liste
menu+0x6C ist heute leer ([dl] count=0); der Builder 0x2800C8 hat 0
Hooks, der Drain 0x28D424 laeuft. Die "96 Draws" sind 4 Ring-Slots x
24 Renderer-Paesse (0x11DB84), keine 96 Sprite-Items.

**A3. BOTTOM (320x240) - erledigt in `88a816c`.**
`HLE_PICA_SECOND=1E100000,320,240,1F060000` führt dieselbe Engine-Liste
ein zweites Mal mit der Bottom-Textur aus (`second pass ... ops=1`).
`HLE_PICA_PRIMARY` bindet vor jedem Submit die Top-Textur neu (sonst zieht
der Top-Draw die Bottom-Textur). BOTTOM 0/71360.

**B. Engine-Stream natürlich an `0x0CFE6000` bringen.**
Statt HLE-Kopie den Engine-Submit/Trigger nutzen (`0x12FD30` -> `0x13E3E0`)
und den Emitter `0x1323FC` echte Vertexdaten (`0x200..0x205`) schreiben
lassen; dann fällt `HLE_PICA_QUAD` weg.

**C. Forcing abbauen.**
`rom:`-Registrierung (0x287BB4 mit echtem ROMFS-Objekt oder 0x100B4C),
Ring-Strides über echten Ctor, `0x12493C`-Gates (`0x26F130`), dann
`HLE_FRAME_LOOP2` und Descriptor-POKEs entfernen.

**D. HLE-Assetpfad komplett löschen** (nur noch Engine), Top/Bottom messen.

## 5. Arbeitsweise - bitte tiefer und disziplinierter als bisher

### 5.1 Skills zuerst

`3ds-hle` und `3ds-port` laden. Rezepte aus `docs/SKILL.md` aktiv nutzen:
`hotpatch.py`, `framecheck.py`, `trace-diff`, `guest-refs`,
`guest-watch`, `HLE_BLKLOGF`, `obj`-Hook. **Rebuild nur**, wenn Scanner,
Emitter oder Runtime-C geändert wurde; sonst Env/Hotpatch.

### 5.2 Sub-Agents parallel (Pflicht pro Runde)

Zu Beginn jeder Runde **2-3 Sub-Agents** (`explore` oder `general`)
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

- `out/romfs_image.bin` - **pristine**, nie überschreiben.
- `out/romfs/` - Spieldaten (read-only), `out/exefs/code_decomp.bin`.
- `out/fa_a/seq/menu_logo.xq` - Byte-Referenz für den ARC0-Index.
- `out/menu_top.bin`, `out/menu_bottom.bin` - nur HLE-Fallback.
- `reports/menu_official.bin`, `reports/menu_official_bottom.bin` -
  Vergleichsreferenz, nicht ändern.
- `out/port.exe` - kanonisches Binary; Build-Varianten wurden gelöscht.
- Profile: `tools/profiles/chain.json` (Kette), `engine_pica_draw.json`
  (Engine-Strom @0x30 -> 0x0CFE6000, TOP 0/90400 diff), `engine_pica_30.json`
  (nur Submit-Beleg), `sprite_bind_gates.json`, `menu_xq_noforce.json`,
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
- `out/menu_top.bin` ist **Rauschen**, kein Menü (mean 96.3 je Kanal, std 68,
  keine Struktur bei 256..1200 px Zeilenbreite; mean-Diff 78.19 zur
  Referenz). Wer den HLE-Assetpfad benutzt, sieht Rauschen.
- Der Strom @`0x30` setzt **keine** Vertex-Loader und **keine**
  Textur-Register; `0x228` (nv) ist unser `HLE_PRECALL`-`r3`. Ohne
  `HLE_PICA_QUAD` malt er nichts.
- `HLE_PICA_LOG` muss gesetzt sein, sonst ist `[pica]` stumm (Default 0).
- `HLE_PRESENT_FILL` (Default an) überschreibt beide Framebuffer mit
  `#604020`; für Render-Nachweise auf `0` setzen.

## 9. Kanonische Befehle

```powershell
# Skills laden (im Agenten-Tool), dann:
python tools\build_port.py
python tests\mvp_selftest.py
python tools\selftest_tools.py

# Kette
python tools\run_chain_direct.py --profile tools\profiles\chain.json --shot out\shot_tmp.bmp
python tools\canon.py --log out\logs\chain_direct.txt --shot out\shot_tmp.bmp --clean

# Engine-PICA-Draw @0x30 (rendert, TOP 0/90400)
python tools\run_chain_direct.py --profile tools\profiles\engine_pica_draw.json --shot out\shot_tmp.bmp
python tools\canon.py --log out\logs\chain_direct.txt --shot out\shot_tmp.bmp --engine --clean

# Messung
python tools\measure_ref.py out\shot_engine.bmp
```

## 10. Offene Fragen für die Sub-Agents

1. Wer setzt den 2D-GX-Kontext (C-Tabelle) - und warum ist `[0x408808+4]=0`?
2. Wie schreibt der Emitter `0x1323FC` echte Vertexdaten (`0x200..0x205`)
   + Textur-Register, damit `HLE_PICA_QUAD` wegfallen kann? (Der Draw
   selbst ist erledigt, Commit `d315412`.)
3. Wie registriert man `rom:` ohne Hänger (`0x100B4C`)?
4. Welche Vtable mit gültigem `+0x78` ist die echte Sprite-Draw-Klasse?
5. Wie kommt der Strom @`0x30` ohne Ring-Poke an `0x0CFE6000`?
