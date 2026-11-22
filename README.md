# Layton 6 (Azran Legacy) — nativer PC-Port

Ziel: *Professor Layton and the Azran Legacy* (3DS, EUR) als **natives** PC-Programm
spielbar machen — ohne Emulator, per **Static Recompilation** des ARM11-Codes
plus Host-Shims für Services, GPU, Audio und Input.

Status: **Phase 0 (Machbarkeit) abgeschlossen** → siehe [`reports/FEASIBILITY.md`](reports/FEASIBILITY.md).

## Kurzbefund

| | |
|---|---|
| ROM | `Professor Layton and the Azran Legacy (Europe).3ds`, 1 GiB, Titel `00040000000F3000` |
| Krypto | komplett entschlüsselt (`no_crypto`) — keine Keys nötig |
| Code | 1,79 MB komprimiert → **3,06 MB / 755.611 ARM-Instruktionen**, ~7.000–8.000 Funktionen |
| Assets | **610 Dateien / 900 MiB**: 3 × Level-5-`ARC0` (425 MiB), 566 × BCSTM (265 MiB), 34 × MOFLEX (190 MiB) |
| SDK | CTR SDK 4.2.4, Mobiclip-Videostack im Spiel integriert (Software-Decode) |

## Quickstart

```powershell
pip install pyctr capstone

python tools\extract.py        # ExeFS -> out\exefs\, RomFS-Listing -> reports\romfs_listing.tsv
python tools\decomp.py         # .code entpacken -> out\exefs\code_decomp.bin (3.059.712 B)
python tools\codestats.py      # Mnemonics / SVC-Statistik
python tools\code_strings.py   # Strings, BL-Ziele, VFP-Nutzung
python tools\exheader.py       # CodeSetInfo + Service-Access-Liste
python tools\assetfmt.py       # Asset-Header (ARC0 / CSAR / CSTM / MOFLEX)
```

## Diagnose-Werkzeuge

```powershell
python tools\where.py --run --thread 2      # Warteschleife + Feld + Schreiber
python tools\callgraph.py writers 0x40B4C4  # statische Programmdatenbank
python tools\diff_trace.py                  # Referenz vs. Port ab gsp 0x001E
python tools\milestones.py                  # Fortschrittskette mit Beweisen
python tools\matrix.py --matrix "HLE_RM_ADV=0|1"
python tools\perfmap.py run --timeout 12
python tools\hotpatch.py run --hook 134818=count --timeout 5
python tools\framecheck.py
```

Rezepte und Fallstricke: [`docs/SKILL.md`](docs/SKILL.md), Details:
[`docs/TOOLS.md`](docs/TOOLS.md).

## Recompiler-MVP (Phase 1)

`tools/recomp/` übersetzt das ARM11-`code.bin` statisch nach C; `runtime/arm_cpu.c`
liefert CPU-Zustand, Speicher und Instruktionssemantik.

| Kennzahl | Wert |
|---|---|
| ARM-/Thumb-Funktionen | 10.215 / 40 |
| Instruktionen | 722.271 (720.643 ARM + 1.628 Thumb) |
| Text-Abdeckung | 71,48 % |
| nicht unterstützt | **0 (0,00 %)** |
| generiertes C | 46,3 MB, kompiliert mit `zig cc` |
| Selbsttests | 13/13 |

Abgedeckt sind auch ARMv6-SIMD (`uadd8`, `uqsub8`, `sel`, …), DSP (`smlabb`, `qadd`,
`pkhbt`), Thumb-Codegen und rotierte Immediates. Vollständiges `code.bin` → C ist
damit übersetzbar; es fehlen nur noch Dispatcher/Speicher-Map (Phase 3).

```powershell
$env:PYTHONPATH = "tools"
python -m recomp.cli scan --in out\exefs\code_decomp.bin --prologues --json reports\recomp_stats_full.json
python -m recomp.cli gen  --in out\exefs\code_decomp.bin --out build\generated_all.c --prologues

python tests\mvp_selftest.py     # assembliert Snippets, recompiliert, prueft Semantik
```

Details: [`reports/RECOMPILER_MVP.md`](reports/RECOMPILER_MVP.md)

## Erste lauffähige Version (Phase 2)

`out\port.exe` startet ein natives Fenster, lädt den recompilierten Code und führt ihn
mit der HLE-Schicht aus. Das **offiziell gerenderte Hauptmenü** wird über den echten
GPU-Pfad angezeigt: `runtime/hle/hle.c` reicht eine Kommandoliste als
`SubmitCmdList`/`SetCommandList` + `TriggerCmdReqQueue` + `SetBufferSwap` durch die
gsp-Handler, `runtime/gpu.c` führt sie aus und schreibt real nach `0x1F000000`
(0 Pixel Unterschied zum Referenzframe, ohne Host-`memcpy`). Details:
[`reports/PORT.md`](reports/PORT.md), [`docs/PORT_STATE.md`](docs/PORT_STATE.md).

```powershell
python tools\build_port.py
python tools\_make_menu_frame.py          # Top- und Bottom-Referenz erzeugen
.\out\port.exe out\romfs --shot out\shot.bmp
python tools\_cmp_shot.py out\shot.bmp reports\menu_official.bin   # diff_pixels=0
```

**Nächstes Ziel:** oberen und unteren 3DS-Screen (400x240 + 320x240)
übereinander anzeigen und beide aus dem Engine-eigenen Buffer rendern
(`SetBufferSwap screen=0/1`). Der Bottom-Referenzframe liegt als
`reports/menu_official_bottom.bin` bereit; der vollständige Auftrag steht in
[`docs/HANDOVER_PROMPT.md`](docs/HANDOVER_PROMPT.md).

## Trace-Harness (Phase 1b)

`runtime/arm_trace.c` + `tools/recomp/trace.py` erzeugen und analysieren
Ausführungs-Traces (Format `CTRTRC01`: Blockstarts, SVCs, IPC, Frames).

```powershell
python tests\self_trace.py          # recompilierten Code ausfuehren und tracen
python -m recomp.trace stats    out\selftrace\run.ctrt
python -m recomp.trace coverage out\selftrace\run.ctrt --in out\exefs\code_decomp.bin --prologues
python -m recomp.cli scan --in out\exefs\code_decomp.bin --prologues --trace out\selftrace\run.ctrt
```

Für Emulator-Traces liegt `patches/azahar-trace.patch` bereit (Hook in
`MemoryReadCode`/`CallSVC`/`HandleSyncRequest`/`SetBufferSwap`); Details in
[`reports/TRACE.md`](reports/TRACE.md).

## Struktur

```
tools/          Analyse, Extraktion, Recompiler (Python)
runtime/        ARM11-CPU-Runtime fuer den generierten C-Code
tests/          Semantik-Selbsttests (keystone + zig cc)
reports/        Machbarkeitsbericht, MVP-Bericht, Statistiken, Referenzframes
docs/           Messrezepte, Werkzeuge, Handoff-Prompt
out/            extrahierte Daten / Testartefakte (nicht versioniert)
  out/logs/     Mess-Logs (Historie)
  out/shots/    Screenshots
  out/probes/   Probe-/Alt-Binaries
  out/pdb/      Debug-Symbole
  out/artifacts/ Traces, .o, .raw
build/          generierter C-Code, Objektdateien (nicht versioniert)
```

## Nächste Schritte

1. **Beide Screens:** 400x480-Fenster, Top 400x240 + Bottom 320x240
   uebereinander; `SetBufferSwap screen=0/1` und getrennte Framebuffer.
2. **Engine-Render:** die Spiel-Engine baut die Szene und submitted ihre
   Kommandoliste selbst (Draw-Liste `fn_002800C8` → `fn_001B9938`/
   `fn_001196AC` → `0x0006`/`0x000C`/`0x0005`); Gates und Adressen stehen im
   [`docs/HANDOVER_PROMPT.md`](docs/HANDOVER_PROMPT.md).
3. Danach: Audio (BCSTM), Saves, Input; Differenztests pro Funktion.

## Rechtliches

Die ROM wird nicht mitgeliefert. Der Port soll seine Daten über einen Extraktor
aus der **eigenen** Cartridge-Kopie des Nutzers beziehen.
