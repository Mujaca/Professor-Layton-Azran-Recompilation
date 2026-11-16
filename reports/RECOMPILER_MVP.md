# Recompiler-MVP — Stand & Ergebnisse

**Ziel:** ARM11-Code aus `code.bin` statisch nach C übersetzen, kompilierbar und
semantisch korrekt — als Fundament für den nativen Port.

Stand: 2026-11-17

---

## 1. Ergebnis in Zahlen

| Kennzahl | Wert |
|---|---|
| ARM-Funktionen erkannt | **10.215** |
| Thumb-Funktionen erkannt | 40 |
| Basic Blocks | **83.085** |
| ARM-Instruktionen übersetzt | **720.643** |
| Abdeckung des Text-Segments | **71,48 %** (2.187.094 von 3.059.712 B) |
| Nicht unterstützte Instruktionen | **11.024 (1,53 %)** |
| Generiertes C | **46,4 MB / 1,65 Mio. Zeilen** |
| Kompilierzeit (`zig cc -O1`, kompletter Korpus) | ~16 s (Compile) / ~88 s (Compile + Link) |
| Ergebnis-Binary (Link-Rauchtest) | 20,8 MB |
| Semantik-Selbsttests | **7/7 bestanden** |

Reiner Entry-Callgraph (ohne Prolog-Heuristik): 922 Funktionen, 62.701 Instruktionen,
davon nur **0,06 %** nicht unterstützt — der ARM-Kern ist also praktisch vollständig.

---

## 2. Was der MVP kann

**Scanner** (`tools/recomp/scan.py`)
- Worklist über `BL`/`BLX`-Ziele ab Einstiegspunkt `0x00100000`
- zusätzliche Seeds über Prolog-Heuristik (`push {…, lr}` / `stmdb sp!, {…, lr}`)
- Basic-Block-Bildung mit korrekter Terminator-Erkennung (B, BX, BLX-reg, `pop {pc}`,
  `ldm … pc`, `ldr/mov/add/sub pc`, `cbz/cbnz`)
- ARM **und** Thumb (Modus wird pro Ziel aus `BLX`/Thumb-Regeln abgeleitet)
- Zählt Aufrufe, Blöcke, besuchte Bytes (Coverage-Metrik)

**Codegenerator** (`tools/recomp/emit.py`)
- Datenverarbeitung inkl. Shifter (imm/reg, LSL/LSR/ASR/ROR/RRX), `S`-Flags
- bedingte Ausführung aller Instruktionen (`if (arm_cond(...))`)
- Multiplikationen (`mul`, `mla`, `umull/umlal/smull/smlal`)
- Loads/Stores (Byte/Half/Word, signed, pre/post-index, Writeback, PC-relativ)
- `ldm/stm` in allen vier Modi, `push/pop`
- `b/bl/bx/blx` inkl. Thumb-Umschaltung bei `blx #imm`
- `svc` (an Host-Hook), `mrs/msr`, `clz`, `movw/movt`
- Extends (`uxth/uxtb/sxth/sxtb`, `uxtah/sxtah`), `usat/ssat`, `rev/rev16/revsh`
- `ldrd/strd`, `ldrex/strex`, `mrc/mcr` (inkl. **TPIDRURO**, dem TLS-Zeiger des 3DS)
- VFP: `vldr/vstr`, `vmov` (Core↔FP, imm), `vadd/vsub/vmul/vdiv`, `vmla/vmls/vnmul`,
  `vneg/vabs/vsqrt`, `vcmp(+e)`, `vcvt` (f32↔f64↔s32/u32), `vmrs/vmsr`,
  `vpush/vpop`, `vldmia/vstmia/vldmdb/vstmdb`

**Runtime** (`runtime/arm_cpu.h`, `runtime/arm_cpu.c`)
- CPU-Zustand (r0–r15, N/Z/C/V, T-Bit, 64×32-Bit-VFP-Register, FPSCR, TPIDRURO)
- Little-Endian-Speicher mit Bounds-Check und Unaligned-Unterstützung
- Zentralisierte Semantik: `arm_dp`, `arm_shift_*`, `arm_mull`, `arm_ldm_stm`,
  `vfp_op`, `vfp_cmp` — dadurch sind Korrekturen an einer Stelle möglich
- Fehlerpfad: `arm_unimplemented()` setzt `aborted`, Aufrufer prüfen das

---

## 3. Selbsttests (`tests/mvp_selftest.py`)

Die Tests assemblieren echten ARM-Code (keystone), recompilieren ihn, kompilieren das
Ergebnis mit `zig cc` und prüfen Register/Flags:

| Test | prüft |
|---|---|
| `schleife_summe` | Flags, `subs`, `bne`, Schleife (r0 = 55) |
| `speicher` | `ldr/str/ldrb`, Offset-Adressierung |
| `aufruf` | `bl` + `bx lr`, Rückkehr, lr-Handling |
| `push_pop_cond` | Stack, `pop {pc}`, bedingte `mov` |
| `shifts` | LSL/LSR im Operanden, Immediate-Rotation |
| `vfp_float` | `vmov` Core↔s0, `vadd.f32` |
| `ldm_stm` | `stmia/ldmia`, Blocktransfers |

Aufruf:

```powershell
python tests\mvp_selftest.py
```

---

## 4. Der verbleibende Gap: ARMv6 SIMD/DSP

1,53 % der Instruktionen fehlen — fast ausschließlich **ARMv6 SIMD-Erweiterungen**
(Byte-/Halfword-Parallelarithmetik), die Layton u. a. für Bild- und Audiobearbeitung nutzt:

| Mnemonic | Vorkommen |
|---|---|
| `uqsub8` | 3174 |
| `sel` | 2724 |
| `uhadd8` | 1326 |
| `uadd8` | 1068 |
| `usub8` | 735 |
| `uqadd8` | 516 |
| `uxtb16` | 492 |
| `ssub8` | 333 |
| `smlabb` | 183 |
| `pkhbt` / `pkhtb` | 303 |

Diese sind mechanisch gut implementierbar (kleine, klar spezifizierte Helfer) — das ist
der nächste kleine Schritt, danach ist der ARM-Pfad bei ~100 %.

**Noch nicht im MVP:**
- Thumb-**Codegen** (40 Funktionen sind derzeit Stubs; Scanner liefert bereits die Grenzen)
- indirekter Aufruf-Dispatcher (`blx rN` setzt PC und kehrt zurück, statt weiterzulaufen)
- CP15-Systemregister außer TPIDRURO, `mcr/mrc`-Seiteneffekte
- VFP-Rundungsmodi/FPSCR-Flags über das Nötigste hinaus
- keine Services, keine Memory-Map des 3DS, keine Zeitsimulation (kommt in Phase 2/3)

---

## 5. Reproduktion

```powershell
$env:PYTHONPATH = "tools"

# Statistik über den ganzen Korpus
python -m recomp.cli scan --in out\exefs\code_decomp.bin --prologues --json reports\recomp_stats_full.json

# C-Code generieren
python -m recomp.cli gen --in out\exefs\code_decomp.bin --out build\generated_all.c --prologues

# Kompilieren (zig cc als C-Compiler)
python -c "import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),'zig.exe'))"
zig cc -std=c11 -O1 -I runtime tests\link_smoke.c build\generated_all.c runtime\arm_cpu.c -o out\link_smoke.exe
```

---

## 6. Nächste Schritte

1. **ARMv6-SIMD-Helfer** ergänzen (`sel`, `*add8/sub8`, `*hadd8`, `uxtb16`, `pkhbt/pkhtb`) → Restgap schließen.
2. **Thumb-Codegen** für die 40 Thumb-Funktionen (Operandensyntax ist zu 80 % identisch;
   Unterschiede: PC-Basis `(pc+4)&~3`, Zwei-Operanden-Formen, IT-Blöcke).
3. **Dispatcher**: Tabelle `addr → fn` erzeugen, `blx rN` und Rückkehr über PC auflösen.
4. **Differenztests gegen Trace** (Phase 1 aus `FEASIBILITY.md`): echte Funktions-Ein-/Ausgaben
   aus Azahar gegen den recompilierten Code prüfen.
5. Danach Phase 3: Speicher-Map, SVC-/Service-Shims, Boot bis Menü.
