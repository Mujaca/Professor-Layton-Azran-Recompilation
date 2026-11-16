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

## Recompiler-MVP (Phase 1)

`tools/recomp/` übersetzt das ARM11-`code.bin` statisch nach C; `runtime/arm_cpu.c`
liefert CPU-Zustand, Speicher und Instruktionssemantik.

| Kennzahl | Wert |
|---|---|
| ARM-Funktionen | 10.215 |
| ARM-Instruktionen | 720.643 |
| Text-Abdeckung | 71,48 % |
| nicht unterstützt | 1,53 % (nur ARMv6-SIMD) |
| generiertes C | 46,4 MB, kompiliert mit `zig cc` |
| Selbsttests | 7/7 |

```powershell
$env:PYTHONPATH = "tools"
python -m recomp.cli scan --in out\exefs\code_decomp.bin --prologues --json reports\recomp_stats_full.json
python -m recomp.cli gen  --in out\exefs\code_decomp.bin --out build\generated_all.c --prologues

python tests\mvp_selftest.py     # assembliert Snippets, recompiliert, prueft Semantik
```

Details: [`reports/RECOMPILER_MVP.md`](reports/RECOMPILER_MVP.md)

## Struktur

```
tools/          Analyse, Extraktion, Recompiler (Python)
runtime/        ARM11-CPU-Runtime fuer den generierten C-Code
tests/          Semantik-Selbsttests (keystone + zig cc)
reports/        Machbarkeitsbericht, MVP-Bericht, Statistiken
out/            extrahierte Daten / Testartefakte (nicht versioniert)
build/          generierter C-Code, Objektdateien (nicht versioniert)
```

## Nächste Schritte

1. Trace-Harness auf Basis von Azahar (Block-, SVC-, IPC-, `fs`-, GPUREG-, Swap-Trace).
2. Recompiler (ARMv6K + VFPv2 → C) mit Differenztests gegen den Emulator.
3. Runtime-Skeleton → Boot bis Hauptmenü, dann 2D-Renderer, Audio, Saves.

## Rechtliches

Die ROM wird nicht mitgeliefert. Der Port soll seine Daten über einen Extraktor
aus der **eigenen** Cartridge-Kopie des Nutzers beziehen.
