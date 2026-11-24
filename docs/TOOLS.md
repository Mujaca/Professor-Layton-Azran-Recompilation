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

Zusaetzlich gezielt abgedeckt (2026-11-24):

* `ldm`/`stm` in allen vier Adressierungsarten mit/ohne Writeback, `^`-Form
  (User-Registerbank; r0-r12) und `pc` in der Liste (Interworking-Sprung),
* `swp`/`swpb`, `ldrt`/`strt`/`ldrbt`/`strbt`/`ldrht`/`strht` (immer
  post-indexiert), negative Register-Offsets (`[r1, -r2, lsl #2]`),
* `mrs`/`msr` auf APSR-Feldern (NZCVQ und GE getrennt; MRS liefert
  zusaetzlich T-Bit und Modusbits 0x10),
* `smmul`/`smmla`/`smmls` (Ra liegt im oberen Wort), `smuad(x)`/`smusd(x)`,
  `smlad(x)`/`smlsd(x)` inkl. Q-Flag bei Ueberlauf, `smlald(x)`/`smlsld(x)`,
  `umaal`, `sxtab16`/`uxtab16` (16-Bit-Halbwort-Addition, ROR),
* Thumb-2-Breitformen mit Shift-Operanden (`add.w ..., ror #n`,
  `mov.w/mvn.w/cmp.w/tst.w ... ror #n`),
* gezielte 32-Bit-Thumb-2-Generatoren (`thumb_t32_cases`): alle 12-Bit-
  modifizierten Immediates inkl. der Muster, die C unberuehrt lassen; Shifts
  (Immediate/Register, 16- und 32-Bit, `rrx`/`rrxs`); `addw`/`subw` und
  SP-Arithmetik; `ldr/str/ldrb/...` mit allen Offsetformen (imm12, imm8
  pre/post, Register mit `lsl #0..3`, negativer Registeroffset). 16-Bit-
  Offsetformen, `[sp]`/`[pc]`, `ldrd`/`strd` (Basis nicht in {Rd,Rd+1}),
  `ldm`/`stm` in allen Modi, `mul.w`/`mla`/`mls`/`umull`/`smull`-Familie,
  Bitfeld-Grenzfaelle (`ubfx/sbfx/bfi/bfc` bei lsb/width 0/31/32, `usat`/
  `ssat`), sowie IT-Bloecke mit allen Befehlsklassen (16-/32-Bit-DP mit und
  ohne S, Shifts, MUL, CMP, Lade/Speichern mit Writeback, LDRD, LDM,
  PUSH/POP, MOVW, SMLABB), alle 15 IT-Masken und bedingte Spruenge im
  IT-Block.

Nicht Teil des Tests: `rbit` (ARM11 ist ARMv6K; RBIT wurde erst mit ARMv6T2/
ARMv7 eingefuehrt) und `setend` (in ARMv6 vorhanden, kommt im Spiel-Image
nicht vor).

```powershell
python tools/recomp_difftest.py --fast            # deterministisch, < 60 s
python tools/recomp_difftest.py --seeds 20 --count 2000 --mode arm
python tools/recomp_difftest.py --seed 7 --count 200
python tools/recomp_difftest.py --mode arm --quick --filter sbc
python tools/recomp_difftest.py --mode cf          # Kontrollfluss-Suite
```

`--fast` ist der feste Regressionlauf (Seed 1, systematische Faelle beider
Modi ohne Zusatz-Zufallsfolgen plus vier Kontrollfluss-Faelle); er umfasst
durch den breiten Thumb-2-Generator inzwischen ~1.900 Faelle und braucht auf
einem Entwicklungsrechner einige Minuten (die Zig-Kompilierung des erzeugten
C-Codes dominiert). `--seeds N` wiederholt denselben Lauf mit
N aufeinanderfolgenden Seeds (breiter Lauf, z. B. `--seeds 20 --count 2000`
je Modus; Laufzeit beachten).

Die Kontrollfluss-Suite (`--mode cf`) baut je Fall ein eigenes Programm aus
mehreren Funktionen (ARM/Thumb gemischt) und vergleicht gegen Unicorn:
bedingte Rueckkehr (`bxcc lr`, `popcc {pc}`, `ldrcc pc`, `movcc pc,lr`),
`ldm sp!,{..,pc}` mit Interworking, `blx reg/imm` in beide Richtungen,
Tail-Calls (`b`), Sprungtabellen (`add pc,pc,rN,lsl #2`,
`ldrls pc,[pc,rN,lsl #2]`, Thumb `tbb`/`tbh` inkl. 16-Bit-Offsets > 255 und
dem maximalen Byte-Offset 0xFF), alle IT-Masken,
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

Der generierte Code bindet die heißen Helfer (`arm_ld`/`arm_st`,
`arm_cond`, `arm_mul`, `arm_shift_imm`/LSL, `arm_restore_sp`) als
`static inline` aus `runtime/cpu/arm_cpu.h` ein; die langsamen Fallbacks
(`*_slow`) behalten Diagnose (ARM_MEMWATCH*) und Fehlzugriffsbehandlung.
Häufige Datenverarbeitung ohne Shift (add/sub/mov/and/orr/bic/eor/cmp/cmn/
tst/teq) emittiert `emit.py::dp_inline` direkt als C-Ausdruck statt als
`arm_dp()`-Aufruf. Blockrate messen: `HLE_PERF=1 out/port.exe out/romfs
--frames 600`.

`tools/func_difftest.py`: **Funktions-Differenztest** auf echten
Gastzustand. `dump` startet den Port mit `ARM_DUMP=0xADDR:datei`; der
Diagnose-Hook in `runtime/cpu/arm_trace.c` schreibt beim ersten Erreichen
des Basisblocks Register, Flags und alle nichtleeren 4-KiB-Seiten des
Gast-Speichers (Codebereich ausgenommen) nach `scratch/`. `diff` laedt den
Schnappschuss in Unicorn, fuehrt dort die Originalbytes ab derselben
Adresse aus und ruft im Port-Build dieselbe `fn_XXXX`-Funktion aus
`build/gen_boot.o` mit identischem Zustand; danach werden Register, Flags
und Speicher verglichen. `--trace` vergleicht zusaetzlich die
Blockeingaenge (PC + Register + Flags je Port-Block).

```powershell
python tools/func_difftest.py dump --watch 0x0016A500 --out scratch/fd/snap.bin
python tools/func_difftest.py diff --snapshot scratch/fd/snap.bin --trace
```

Exit-Code 0 = identisch, 1 = Abweichung (oder der Port terminiert nicht,
wo das Original zurueckkehrt). Das Werkzeug veraendert keinen Gastzustand;
der Schnappschuss ist reine Diagnose (`ARM_DUMP`).

`tools/cosim.py`: **Co-Simulation im Blockgleichschritt**. `snapshot` startet
`port.exe` mit `ARM_DUMP=0xADDR:datei` (Diagnose, nur lesend) und sichert
Register, Flags und Gast-Speicher beim ersten Erreichen eines Basisblocks.
`run` fuehrt die Originalbytes ab derselben Adresse in Unicorn aus (jede
Instruktion gehookt) und ruft im Port-Build (`build/gen_boot.o`) dieselbe
Funktion mit identischem Zustand auf; der Harness zeichnet jeden Basisblock
mit r0-r15 und Flags auf. Beide Blockfolgen werden verglichen; bei der ersten
Abweichung werden Block, Vorgaengerblock (disassembliert), Register/Flags und
ueber einen zweiten Lauf mit Blocklimit die Speicherseiten an dieser
Blockgrenze ausgegeben. `--root` erlaubt einen fremden Arbeitsbaum.

```powershell
python tools/cosim.py snapshot --root . --watch 0x0016A500 --out scratch/cosim/snap.bin
python tools/cosim.py run --root . --snapshot scratch/cosim/snap.bin --context 12
```

Exit-Code 0 = Blockfolge, Register, Flags und Endzustand identisch, 1 =
Abweichung. Reines Testwerkzeug (Unicorn nur hier, nicht im Produkt); es
veraendert keinen Gastzustand.

`tools/cosim/sweep.py`: **Funktions-Sweep** auf echten Gastzustand. Ein
Portlauf mit `HLE_PERFMAP_FILE` (Standard 1200 Frames) liefert
`(pc, thread, count)` je Basisblock; geschnitten mit der Eintragstabelle
`{ 0xADDRu, fn_ADDR }` aus `build/gen_boot.c` ergibt das die ausgefuehrten
Funktionseinstiege. Plan-Reihenfolge: selten/haupt (Hauptthread, `<= --rare`
Laeufe), heiss/haupt, selten/neben, heiss/neben. Je Funktion wird per
`ARM_DUMP` am Einstieg ein Schnappschuss genommen (der Port wird beendet,
sobald die Datei stabil ist) und mit `func_difftest`-Technik blockgleich-
schrittig gegen Unicorn verglichen (Port-Code aus `build/gen_boot.o`,
Zeitlimit `--per-func`, Gesamtbudget `--budget`). Klassen: `gleich`,
`unicorn-ende:svc|tls|mem` (kein Befund), `abweichung:block|ende`,
`port-limit`, `snapshot-fehlend`. Ein Vorgaengerblock mit
`ldrex`/`strex`/`swp` markiert die Abweichung als Lock-/Fremdthread-Verdacht.
Unicorn laeuft mit dem Gastkontext (TLS, FPSCR, `FPEXC.EN`, CP10/CP11 in
CPACR), sonst endet der Vergleich an der ersten VFP-Instruktion.
Bericht: `scratch/sweep/report.md`, Einzelbefunde `fail_XXXXXXXX.txt`.

```powershell
python tools/cosim/sweep.py                                  # Perfmap + Sweep
python tools/cosim/sweep.py --reuse-perf out/cosim/perfmap.txt --max-funcs 20
python tools/cosim/sweep.py --sort addr --max-funcs 200       # Fortsetzung
python tools/cosim/sweep.py --only 0x0028F04C,0x0016A500
```

Reines Testwerkzeug (Unicorn nur hier), aendert keinen Gastzustand.

Zusatz-Diagnose im Port: `ARM_DUMP_LIST=0xADDR,...` + `ARM_DUMP_DIR=<verz>`
schreibt je Adresse hoechstens einen `ARMDMP01`-Schnappschuss nach
`<verz>/dump_XXXXXXXX.bin` (ein Lauf, viele Einstiege; nur lesend).

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
`ARM_WATCH=<hex>` (Treffer werden ab 0 nummeriert; zusaetzlich je 8 Woerter
ab `r0`, `r1` und `r5` als `[watchmem]`-Zeile), `ARM_MEMWATCH=<hex>`,
`ARM_MEMWATCHRD=<hex>[,size]`,
`ARM_MEMWATCHRANGE=<hex>,<size>`, `ARM_STRICT=1`,
`HLE_PERFMAP_FILE=<pfad>`, `HLE_DET_SCHED=1`,
`HLE_PERF=1` (zählt Gast-Bloecke und meldet beim Prozessende
`[perf] N blocks in M ms (R/s)`; Format von `tools/g3ds/runner.py`),
`HLE_PICA_DIAG=1` (PICA-Register-Histogramm je Liste inkl. Kanal-Spruengen,
Rohdump nach `scratch/pica/list_<addr>_<size>.bin`),
`HLE_YSTAT=1` (Yield-/Lock-Statistik der kooperativen Gast-Threads: Yields,
Wandzeit in Lock-Abgaben, Zeit mit freiem Gast-Lock; je ~1 s eine Zeile
`[ystat]`),
`HLE_STALLDUMP=1` (bei jedem 300. Host-Frame die letzten 12 Bloecke je
Gast-Thread auf stderr, um Haenger ohne IPC zu lokalisieren),
`HLE_Y2R_LOG=1` (jede `StartConversion` mit Zeitstempel/Format auf stderr;
zaehlt den Video-Framepfad ohne das langsame `HLE_LOG`),
`HLE_TICK_LOG=1` (die ersten 200 `svcGetSystemTick`-Aufrufe mit Thread,
Block und LR), `HLE_SLEEP_LOG=1` (die ersten 200 `svcSleepThread`-Aufrufe),
`HLE_WAITTMO_LOG=1` (erschöpfte endliche Wartezeiten mit Thread/Block/LR),
`HLE_GTIME=1` (Abschlussbericht `[gtime]`: Wandzeit der PICA-Kommandolisten,
Anteil an der Laufzeit, Fragmente/Textursamples/Lighting/Farbziel-Zugriffe
sowie parallele und serielle Dreieckszeilen des Software-Rasterizers),
`HLE_RASTER_THREADS=<n>` (Worker-Threads fuer das parallele Rasterisieren
grosser Dreiecke; Standard 3, `0` = seriell und pixelgleich).
`HLE_STALLDUMP=1` (bei jedem 300. Host-Frame die letzten 12 Bloecke, den
letzten Registerstand (`r0`–`r12`, `sp`, `lr`), acht Woerter an `r0`/`r1`/`r12`
sowie 40 Stackwoerter je Gast-Thread auf stderr, um Haenger ohne IPC zu
lokalisieren),
`ARM_WATCH_DUMP="n,addr,size[,addr,size …]"` (beim n-ten `ARM_WATCH`-Treffer
bis zu vier Gastspeicherbereiche nach `out/watchdump_<addr>.bin` und alle
Register nach `out/watchdump_regs.txt`; ab dann schreibt derselbe Gast-Thread
je Blockanfang `pc r0–r14 NZCV` nach `out/btrace.txt`,
`ARM_BTRACE_MAX=<n>` Bloecke, Standard 40000).

## Co-Simulation gegen Unicorn (`tools/cosim/`)

Findet Recompiler-Fehler in echtem Spielcode: Speicherabbild und Blockspur des
Ports an einem Funktionseinstieg, dieselbe Ausfuehrung in Unicorn auf den
Originalbytes, Vergleich Block fuer Block (pc, Register, Flags; Versatz durch
inline uebernommene Bloecke wird toleriert).

```powershell
$env:W="0x17A6A0"; $env:N="64"; $env:START="17A6A0"; bash tools/cosim/lockstep_run.sh
```

* `replay.py <dumpdir>`: einen Aufruf ab dem Abbild in Unicorn nachrechnen
  (Ende an der echten Ruecksprungadresse; meldet den hoechsten Lesezugriff ab
  `r1`).
* `lockstep.py <dumpdir> <btrace>`: erste Abweichung Port/Unicorn.
* `moflex.py <datei.moflex>`: unabhaengiger MOFLEX-Parser (Bloecke, Teilpakete,
  Bildgroessen je Stream).

Grenzen: Unicorn fuehrt keine SVCs aus (Lauf endet am ersten `svc`/IPC-Puffer
im TLS); Abweichungen an Speicher, den andere Gast-Threads gleichzeitig
aendern (Locks), sind keine Recompiler-Fehler.

## Assets

`tools/xfsatool/` (Level-5-ARC0/XPCK), `tools/list_xpck.py`,
`tools/scan_xpck.py`, `tools/scan_pvb.py`, `tools/fa_index.py`,
`tools/imgc_check.py`, Test `tools/test_imgc_c.py`, PICA-Test `tools/test_pica_c.py`.

## Referenz

* `tools/azahar` – Azahar-Quellcode (Submodul, **nur lesen**).
* `tools/trace/azahar/` + `patches/azahar-trace.patch` – Trace-Hook für
  Referenzläufe; Ergebnis in `reports/azahar_trace.txt`.
