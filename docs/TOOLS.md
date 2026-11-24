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
python tools/recomp_difftest.py   # Differenztest gegen Unicorn (ARM + Thumb-2)
python tools/vfp_difftest.py      # Differenztest VFP/Float gegen Unicorn
```

`tools/recomp_difftest.py`: erzeugt systematische und zufaellige
ARM-/Thumb-2-Folgen (Datenverarbeitung mit allen Shifts/S-Flags, adc/sbc/rsc,
mul/mla/umull/smull/smlal, clz/rev/extend, ubfx/sbfx/bfi/bfc, Saettigung,
Parallel-/Medienbefehle, ldr/str/ldrd/strd/ldm/stm mit allen Adressierungsarten,
bedingte Ausfuehrung inkl. IT-Bloecken). Jeder Fall laeuft einmal in Unicorn
(Referenz) und einmal als recompilierter C-Code; verglichen werden r0-r14,
NZCVQ/GE und ein Speicher-Digest.

```powershell
python tools/recomp_difftest.py --seed 7 --count 200
python tools/recomp_difftest.py --mode arm --quick --filter sbc
python tools/recomp_difftest.py --mode cf          # Kontrollfluss-Suite
```

Die Kontrollfluss-Suite (`--mode cf`) baut je Fall ein eigenes Programm aus
mehreren Funktionen (ARM/Thumb gemischt) und vergleicht gegen Unicorn:
bedingte Rueckkehr (`bxcc lr`, `popcc {pc}`, `ldrcc pc`, `movcc pc,lr`),
`ldm sp!,{..,pc}` mit Interworking, `blx reg/imm` in beide Richtungen,
Tail-Calls (`b`), Sprungtabellen (`add pc,pc,rN,lsl #2`,
`ldrls pc,[pc,rN,lsl #2]`, Thumb `tbb`/`tbh`), alle IT-Masken,
`cbz`/`cbnz`-Schleifen und Fallthrough zwischen Funktionseinstiegen.
Verglichen werden r0-r14, NZCVQ/GE und der Daten-Digest.

Neue Semantikfehler zuerst hier reproduzieren und dann als festen Fall in
`tests/mvp_selftest.py` aufnehmen.

`tools/vfp_difftest.py`: prueft die VFP-/Float-Semantik gegen Unicorn.
Erzeugt Arithmetik (f32/f64: add/sub/mul/div, abs/neg/sqrt), die
Multiply-Accumulate-Familie, alle VCVT-Richtungen inkl. Festkomma (`#fbits`),
VCMP/VCMPE + `vmrs APSR_nzcv`, `vmov` zwischen Kern- und VFP-Registern sowie
`vldr`/`vstr`/`vldm`/`vstm`/`vpush`/`vpop`. Jeder Fall laeuft mit mehreren
FPSCR-Varianten (3DS-Default `0x03C00010`: Default-NaN, FTZ, Rundung Richtung
Null; sowie IEEE-nearest, `+inf`/`-inf`, ohne FTZ/DN). Verglichen werden alle
s32/s64-Register, Kernregister, SP, FPSCR.NZCV und der Speicher-Digest.

```powershell
python tools/vfp_difftest.py --seed 7 --count 400
python tools/vfp_difftest.py --filter vcvt --count 200
```

NaN-Weitergabe der VMLA-Familie ist architektonisch
implementierungsabhaengig; der Generator verwendet dort keine NaN-Eingaben
(VCMP/VCVT/Arithmetik decken NaN-Faelle ab).

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

## Weitere Tests (bewusst nicht in `tools/check.py`)

| Befehl | Zweck |
|---|---|
| `python tests/hid_shmem_test.py` | HID-Shared-Memory-Layout (Offsets/Groessen/Takt) gegen Azahar `hid.h`, liest `runtime/platform/hid_ring.h` |
| `python tests/hid_ring_test.py` | HID-Ring-Verhalten: Index-Umlauf (8/8/8/32), Tastenbits, Circle-Pad-Richtungsbits, `delta_additions`/`delta_removals`, Ereignis-Signalisierung, Touch ohne/mit Druck, Accelerometer-Ruhelage |
| `python tests/framebuffer_test.py` | Bildausgabe/FrameBufferInfo mit synthetischen Puffern |
| `python tests/y2r_test.py` | YUV→RGB-Konverter mit synthetischen Bildern |
| `python tests/fs_archive_test.py` | fs-Archive (SaveData/Ext/Shared/System): Host-Layout, Result-Codes, Datei-/Verzeichnisoperationen, Directory-Eintraege |
| `python tests/audio_test.py` | DSP-HLE-Dekodierung/Resampling (ADPCM inkl. s16-Verlauf, PCM8/16, lineare Interpolation mit Azahar-Rundung) |
| `python tests/dsp_hle_test.py` | DSP-HLE-Frame-Verarbeitung: Regionenwahl ueber Frame-Counter (inkl. Wraparound), Dirty-Bits, Status-Rueckschreiben, `u32_dsp`, Sleep-Reply nach zwei Frames, DSP-RAM-Nullung bei Initialize |

Diese Tests brauchen nur `zig cc` und laufen in Sekunden; sie gehoeren
absichtlich nicht zum Commit-Gate.

## Diagnose-Umgebungsvariablen (verändern keinen Gastzustand)

`HLE_LOG=1`, `HLE_SRV_NAMES=1`, `HLE_IPC_LOG=<n>`, `HLE_TRACE=1`,
`ARM_WATCH=<hex>`, `ARM_MEMWATCH=<hex>`, `ARM_MEMWATCHRD=<hex>[,size]`,
`ARM_MEMWATCHRANGE=<hex>,<size>`, `ARM_STRICT=1`,
`HLE_PERFMAP_FILE=<pfad>`, `HLE_DET_SCHED=1`,
`HLE_PICA_DIAG=1` (PICA-Register-Histogramm je Liste inkl. Kanal-Spruengen,
Rohdump nach `scratch/pica/list_<addr>_<size>.bin`),
`HLE_YSTAT=1` (Yield-/Lock-Statistik der kooperativen Gast-Threads: Yields,
Wandzeit in Lock-Abgaben, Zeit mit freiem Gast-Lock; je ~1 s eine Zeile
`[ystat]`),
`HLE_STALLDUMP=1` (bei jedem 300. Host-Frame die letzten 12 Bloecke je
Gast-Thread auf stderr, um Haenger ohne IPC zu lokalisieren).

## Assets

`tools/xfsatool/` (Level-5-ARC0/XPCK), `tools/list_xpck.py`,
`tools/scan_xpck.py`, `tools/scan_pvb.py`, `tools/fa_index.py`,
`tools/imgc_check.py`, Test `tools/test_imgc_c.py`, PICA-Test `tools/test_pica_c.py`.

## Referenz

* `tools/azahar` – Azahar-Quellcode (Submodul, **nur lesen**).
* `tools/trace/azahar/` + `patches/azahar-trace.patch` – Trace-Hook für
  Referenzläufe; Ergebnis in `reports/azahar_trace.txt`.
