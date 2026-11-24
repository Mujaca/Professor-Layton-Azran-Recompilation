# Werkzeuge

Alle Python-Tools liegen in `tools/`. Wegwerfskripte gehören nach `scratch/`
(nicht versioniert).

## Pipeline

| Befehl | Zweck |
|---|---|
| `python tools/extract.py` | ExeFS aus der ROM → `out/exefs/`, RomFS-Listing |
| `python tools/extract_romfs.py` | RomFS-Dateien → `out/romfs/` |
| `python tools/romfs_image.py` | SelfNCCH-RomFS-Image → `out/romfs_image.bin` |
| `python tools/build.py` | Patches → Recompiler → C → `out/port.exe` (`--no-gen`, `--pristine`) |
| `python tools/frontier.py` | **Fortschrittsmetrik**: natürlicher Boot gegen Azahar-Referenz |
| `python tools/check.py` | Gate vor jedem Commit (Tests + Build + Frontier) |

## Recompiler (`tools/recomp`)

```powershell
$env:PYTHONPATH = "tools"
python -m recomp.cli scan --in out/exefs/code_decomp.bin --prologues --json reports/recomp_stats_full.json
python -m recomp.cli gen  --in out/exefs/code_decomp.bin --out build/gen_boot.c --prologues --seed-pointers --trace --seed-file tools/recomp/seeds.txt
python tests/mvp_selftest.py      # Instruktionssemantik (keystone + zig cc)
```

`tools/recomp/seeds.txt`: zusätzliche Funktionseinstiege (z. B. aus
`out/missing_calls.txt`).

## Workarounds (`tools/workarounds`) – siehe docs/WORKAROUNDS.md

| Datei | Inhalt |
|---|---|
| `code_patches.json` + `apply.py` | Patches am Spielcode, prüft Originalbytes |
| `block_hooks.json` | C-Zeilen, die der Recompiler an Blöcken/nach Aufrufen einfügt |
| `gen_patches.py` | Textersetzungen in `build/gen_boot.c` (derzeit leer) |

## Analyse (`tools/g3ds.py <befehl>`)

| Befehl | Zweck |
|---|---|
| `trace-diff <azahar.txt> <port.log> --table` | erste Divergenz der Dienstfolge |
| `diff-trace --ref ... --port ... --kind all` | ereignisgenauer Vergleich |
| `guest-refs 0xADDR` | statische Lese-/Schreibzugriffe auf eine Adresse |
| `callgraph writers/literals 0xADDR` | Aufrufer, Schreiber, Literal-Nutzung |
| `guest-run --until "<regex>"` | Port starten bis Logmuster |
| `guest-watch --write 0xADDR` | Laufzeit-Schreibzugriffe (ARM_MEMWATCH) |
| `guest-exp record/check` | Hypothesen-Experimente protokollieren |
| `where --run --thread n` | Wo hängt ein Gast-Thread? |
| `milestones`, `matrix`, `perfmap`, `framecheck` | Meilenstein-Check, A/B-Matrix, Profil, Bildcheck |

Selbsttest: `python tools/selftest_tools.py`.

## Diagnose-Umgebungsvariablen (verändern keinen Gastzustand)

`HLE_LOG=1`, `HLE_SRV_NAMES=1`, `HLE_IPC_LOG=<n>`, `HLE_TRACE=1`,
`ARM_WATCH=<hex>`, `ARM_MEMWATCH=<hex>`, `ARM_MEMWATCHRD=<hex>[,size]`,
`ARM_MEMWATCHRANGE=<hex>,<size>`, `ARM_STRICT=1`,
`HLE_PERFMAP_FILE=<pfad>`, `HLE_DET_SCHED=1`.

## Assets

`tools/xfsatool/` (Level-5-ARC0/XPCK), `tools/list_xpck.py`,
`tools/scan_xpck.py`, `tools/scan_pvb.py`, `tools/fa_index.py`,
`tools/imgc_check.py`, Test `tools/test_imgc_c.py`, PICA-Test `tools/test_pica_c.py`.

## Referenz

* `tools/azahar` – Azahar-Quellcode (Submodul, **nur lesen**).
* `tools/trace/azahar/` + `patches/azahar-trace.patch` – Trace-Hook für
  Referenzläufe; Ergebnis in `reports/azahar_trace.txt`.
