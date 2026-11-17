# Recompiler-MVP — Stand & Ergebnisse

**Ziel:** ARM11-Code aus `code.bin` statisch nach C übersetzen, kompilierbar und
semantisch korrekt — als Fundament für den nativen Port.

Stand: 2026-11-17 (nach SIMD/DSP- und Thumb-Codegen)

---

## 1. Ergebnis in Zahlen

| Kennzahl | Wert |
|---|---|
| ARM-Funktionen erkannt | **10.215** |
| Thumb-Funktionen erkannt | **40** |
| Basic Blocks | **83.085** |
| ARM-Instruktionen übersetzt | **720.643** |
| Thumb-Instruktionen übersetzt | **1.628** |
| **nicht unterstützte Instruktionen** | **0 (0,00 %)** |
| Abdeckung des Text-Segments | **71,48 %** (2.187.094 von 3.059.712 B) |
| Generiertes C | **46,3 MB / 1,80 Mio. Zeilen** |
| Compile + Link (`zig cc -O1`, kompletter Korpus) | ~98 s |
| Ergebnis-Binary (Link-Rauchtest) | 21,6 MB |
| Semantik-Selbsttests | **13/13 bestanden** |

Reiner Entry-Callgraph (ohne Prolog-Heuristik): 922 Funktionen, 62.701 Instruktionen,
davon 0,06 % nicht unterstützt — der ARM-Kern war schon vor dem SIMD-Ausbau praktisch vollständig.

---

## 2. Was der Recompiler kann

**Scanner** (`tools/recomp/scan.py`)
- Worklist über `BL`/`BLX`-Ziele ab Einstiegspunkt `0x00100000`
- zusätzliche Seeds über Prolog-Heuristik (`push {…, lr}` / `stmdb sp!, {…, lr}`)
- Basic-Block-Bildung mit korrekter Terminator-Erkennung (B, BX, BLX-reg, `pop {pc}`,
  `ldm … pc`, `ldr/mov/add/sub pc`, `cbz/cbnz`)
- **ARM und Thumb**, Modus wird pro Ziel aus `BLX`/Thumb-Regeln abgeleitet
- Zählt Aufrufe, Blöcke, besuchte Bytes (Coverage-Metrik)

Die Coverage von 71,48 % erklärt sich aus indirekten Aufrufen (Funktionszeiger,
Vtables, Sprungtabellen): nicht besuchte Bytes werden in Phase „Trace“ aus den
Azahar-Logs nachgezogen, nicht geraten.

**Codegenerator** (`tools/recomp/emit.py`, `tools/recomp/simd.py`)
- Datenverarbeitung inkl. Shifter (imm/reg, LSL/LSR/ASR/ROR/RRX), `S`-Flags,
  condition codes, *rotierte Immediates* (`#imm8, #rot`)
- Multiplikationen (`mul`, `mla`, `umull/umlal/smull/smlal`)
- Loads/Stores (Byte/Half/Word, signed, pre/post-index, Writeback, PC-relativ)
- `ldm/stm` in allen vier Modi, `push/pop`, `ldrd/strd`, `ldrexd/strexd`
- `b/bl/bx/blx` inkl. Thumb-Umschaltung und korrektem LR-Thumb-Bit
- `svc` (Host-Hook), `mrs/msr`, `clz`, `movw/movt`, `adr`
- Extends (`uxth/uxtb/sxth/sxtb`, `uxtah/sxtah`, `uxtb16/sxtb16`),
  `usat/ssat/ssat16/usat16`, `rev/rev16/revsh`
- **ARMv6-SIMD:** `uadd8/uqsub8/sel/uhadd8…` und die 16-Bit-Varianten (alle 12 Ops),
  inkl. GE-Flags und Bytes/Halfword-Sättigung
- **DSP:** `qadd/qsub/qdadd/qdsub`, `smlabb/smlabt/smlatb/smlatt`,
  `smulbb/smulbt/smultb/smultt`, `usad8/usada8`, `pkhbt/pkhtb` (inkl. ASR-Sonderfall)
- CP15 `TPIDRURO` (TLS-Zeiger des 3DS), `mrc/mcr` tolerant, `cdp/ldc/stc/mcrr/mrrc` als No-op
- VFP: `vldr/vstr`, `vmov` (Core↔FP, imm), `vadd/vsub/vmul/vdiv`, `vmla/vmls/vnmla/vnmls/vnmul`,
  `vneg/vabs/vsqrt`, `vcmp(+e)`, `vcvt` (f32↔f64↔s32/u32), `vmrs/vmsr`,
  `vpush/vpop`, `vldmia/vstmia/vldmdb/vstmdb`

**Thumb-Besonderheiten, die der Emitter behandelt**
- PC-Basis `(pc + 4) & ~3` (statt `pc + 8`)
- Zwei-Operanden-Formen (`add r4, pc` → `r4 = r4 + pc`; `subs r0, #8`)
- `cbz/cbnz`, `adr`, LR mit gesetztem Thumb-Bit, `movs`-Alias (capstone-eigene ID)
- 16- und 32-Bit-Encodings gemischt im selben Block

**Runtime** (`runtime/arm_cpu.h`, `runtime/arm_cpu.c`)
- CPU-Zustand (r0–r15, N/Z/C/V, GE[3:0], Q, T-Bit, 64×32-Bit-VFP, FPSCR, TPIDRURO)
- Little-Endian-Speicher mit Bounds-Check und Unaligned-Unterstützung
- Zentralisierte Semantik-Helfer (`arm_dp`, `arm_shift_*`, `arm_mull`, `arm_ldm_stm`,
  `arm_par8/16`, `arm_sel`, `arm_smla16`, `arm_qadd32`, `arm_usad`, `vfp_*`) —
  Korrekturen passieren an einer Stelle
- Fehlerpfad: `arm_unimplemented()` setzt `aborted`, Aufrufer prüfen das

---

## 3. Selbsttests (`tests/mvp_selftest.py`)

Die Tests assemblieren echten ARM-/Thumb-Code (keystone), recompilieren ihn, bauen das
Ergebnis mit `zig cc` und prüfen Register/Flags:

| Test | prüft |
|---|---|
| `schleife_summe` | Flags, `subs`, `bne`, Schleife (r0 = 55) |
| `speicher` | `ldr/str/ldrb`, Offset-Adressierung |
| `aufruf` | `bl` + `bx lr`, Rückkehr, lr-Handling |
| `push_pop_cond` | Stack, `pop {pc}`, bedingte `mov` |
| `shifts` | LSL/LSR im Operanden, Immediate-Rotation |
| `vfp_float` | `vmov` Core↔s0, `vadd.f32` |
| `simd_uadd8_sel` | Byte-Paralleladdition, GE-Flags, `sel` |
| `simd_saturierung` | `uqadd8`/`uqsub8` Sättigung |
| `dsp_smlabb` | 16-Bit-Multiply-Accumulate, `smulbb` |
| `pack_und_extend` | `pkhbt` mit Shift, `uxtb16` |
| `rotated_immediate` | rotierter Immediate-Operand (`add r6, pc, #imm8, #rot`) |
| `thumb_schleife` | Thumb-Decode + Codegen (2-/4-Byte gemischt) |
| `ldm_stm` | `stmia/ldmia`, Blocktransfers |

Aufruf:

```powershell
python tests\mvp_selftest.py
```

---

## 4. Nächste Lücken

- **indirekter Aufruf-Dispatcher**: `blx rN` setzt PC und kehrt zurück; es fehlt die
  Tabelle `addr → fn` im generierten Code (Phase 3, zusammen mit der Speicher-Map)
- **Thumb-Randfälle**: IT-Blöcke, `tbb/tbh`-Sprungtabellen, Thumb-2-`ldrd/strd`-Formen
  (im aktuellen Korpus nicht vorhanden, aber bei anderen Layton-Titeln möglich)
- **CP15-Seiteneffekte** außer TPIDRURO, echte `mcr`-Wirkung
- **VFP-Fidelity**: Rundungsmodi/FPSCR-Ausnahmen über das Nötigste hinaus
- **Coverage 71 % → 100 %**: über den Azahar-Trace (Block-Log) die indirekt
  erreichten Funktionen nachziehen, dann gegen echte Ein-/Ausgaben differenztesten

---

## 5. Reproduktion

```powershell
$env:PYTHONPATH = "tools"

python -m recomp.cli scan --in out\exefs\code_decomp.bin --prologues --json reports\recomp_stats_full.json
python -m recomp.cli gen  --in out\exefs\code_decomp.bin --out build\generated_all.c --prologues

python tests\mvp_selftest.py
zig cc -std=c11 -O1 -I runtime tests\link_smoke.c build\generated_all.c runtime\arm_cpu.c -o out\link_smoke.exe
```

---

## 6. Nächste Schritte

1. **Trace-Harness (Phase 1)** auf Basis von Azahar: Block-, SVC-, IPC-, `fs`-,
   GPUREG- und Swap-Trace → schließt die Coverage-Lücke und liefert Diff-Test-Daten.
2. **Dispatcher + Speicher-Map** (Phase 3): Adresstabelle, FCRAM-Modell +
   VRAM, SVC-/Service-Shims, Boot bis Menü.
3. **GPU-Shim** für die im Trace gefundenen TEV-/Shader-Konfigurationen.
