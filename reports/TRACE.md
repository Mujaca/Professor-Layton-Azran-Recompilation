# Trace-Harness

Zweck: **Ausführungsdaten** sammeln, um (a) die Scanner-Coverage von 71,5 % auf
nahe 100 % zu heben und (b) den recompilierten Code gegen den Emulator zu
differenztesten.

Stand: 2026-11-17

---

## 1. Format (`CTRTRC01`)

8-Byte-Kopf `CTRTRC01`, danach Little-Endian-Records:

| Tag | Payload | Bedeutung |
|---|---|---|
| `B` | `u32 pc`, `u8 thumb` | Blockstart (JIT-Übersetzung bzw. Ausführung) |
| `S` | `u32 imm` | SVC-Nummer |
| `I` | `u8 len`, `name[len]`, `u16 cmd` | IPC-Request (Service + Command-ID) |
| `F` | `u32 index` | Frame-/Buffer-Swap |
| `E` | `u32 kind`, `u32 a`, `u32 b` | freies Ereignis (z. B. unimplemented) |

Leser/Analyse: `tools/recomp/trace.py` — `read`, `entry_points`, `stats`,
`coverage`, `diff`.

---

## 2. Produzent A: Azahar-Patch

`patches/azahar-trace.patch` (gegen Azahar HEAD `11eceda`, verifiziert durch
Einbau im Quellbaum) ergänzt:

| Datei | Hook |
|---|---|
| `src/core/trace/ctr_trace.{h,cpp}` | neu: gepufferter Writer, `std::mutex`-geschützt |
| `src/core/arm/dynarmic/arm_dynarmic.cpp` | `MemoryReadCode()` → Blockstart + Thumb-Bit aus `GetCPSR()`; `CallSVC()` → SVC |
| `src/core/hle/service/service.cpp` | `ServiceFrameworkBase::HandleSyncRequest()` → IPC (Service, Command-ID) |
| `src/core/hle/service/gsp/gsp_gpu.cpp` | `GSP_GPU::SetBufferSwap()` (screen 0) → Frame |
| `src/core/CMakeLists.txt` | Quellen registriert |

`MemoryReadCode` wird von Dynarmic beim Übersetzen eines Blocks aufgerufen — also
genau einmal pro ausgeführtem Block, ohne Per-Instruktions-Overhead.

Anwendung:

```powershell
git clone --depth 1 https://github.com/azahar-emu/azahar
cd azahar
git apply <pfad>\patches\azahar-trace.patch
# bauen (MSVC + CMake, siehe Azahar-Doku)
$env:CTR_TRACE_FILE = "C:\Users\user\Documents\Repos\3DS\out\azahar.ctrt"
azahar.exe    # Spiel bis ins Menü / durch ein Kapitel spielen
```

Hinweis: Der Patch wurde hier **nicht kompiliert** (in dieser Umgebung fehlen
MSVC/CMake/Qt). Er ist gegen den aktuellen Quellstand eingebaut und diffbar; die
Hook-Stellen sind bewusst klein gehalten, damit er auch bei neueren Revisionen
leicht anzupassen ist.

## 3. Produzent B: Selbst-Trace (ohne Emulator)

Der recompilierte Code kann sich selbst tracen:

```powershell
python tests\self_trace.py
```

Das Skript generiert C mit `--trace`, baut einen Host (512 MB–768 MB flaches
Speicherabbild, SVCs als No-op), führt `fn_00100000` aus und wertet den Trace aus.

Gemessener Demo-Lauf:

| Kennzahl | Wert |
|---|---|
| Trace-Datei | 0,7 KiB (120 eindeutige Blöcke, dedupliziert) |
| Lauf endet | `aborted=1`, PC `0x00281CF0` (MMIO-Zugriff außerhalb des Modells) |
| `scan --trace` danach | 10.259 ARM- + 44 Thumb-Funktionen (**+44 / +4** gegenüber Prolog-Scan) |

Damit ist die Kette **Trace → Seeds → Scanner** nachgewiesen. Die großen Sprünge
kommen mit dem Azahar-Trace (kompletter Durchlauf).

Die Runtime dedupliziert Blockadressen standardmäßig (`ARM_TRACE_ALL=1` schaltet
Ausführungszählung ein, `ARM_TRACE_MAX=<n>` begrenzt die Recordzahl).

---

## 4. Workflow

```powershell
$env:PYTHONPATH = "tools"

# 1) Trace auswerten
python -m recomp.trace stats    out\azahar.ctrt
python -m recomp.trace coverage out\azahar.ctrt --in out\exefs\code_decomp.bin --prologues

# 2) Coverage mit Trace-Seeds schließen und neu recompilieren
python -m recomp.cli scan --in out\exefs\code_decomp.bin --prologues --trace out\azahar.ctrt
python -m recomp.cli gen  --in out\exefs\code_decomp.bin --out build\generated_all.c --prologues --trace

# 3) Zwei Traces vergleichen (z. B. Emulator vs. Port)
python -m recomp.trace diff out\azahar.ctrt out\port.ctrt
```

`diff` meldet Anzahl Blöcke, Mengendifferenzen und die **erste Divergenz** im
Blockstrom (Adresse + ARM/Thumb) — genau der Einstiegspunkt für die Fehlersuche,
wenn der Port an einer Stelle anders abbiegt als der Emulator.

---

## 5. Was der Trace als Nächstes liefert

1. **Coverage**: alle indirekt erreichten Funktionen werden zu Seeds → Scanner
   soll von 71,5 % auf >95 % springen; fehlende Blöcke sind dann echte
   Analyse-Lücken statt Callgraph-Blindflug.
2. **Service-Inventar**: `stats` zeigt, welche IPC-Commands tatsächlich benutzt
   werden (Reihenfolge/Häufigkeit) — direkte Priorisierung für die HLE-Shims.
3. **Differenztests**: pro Funktion Ein-/Ausgabewerte aus dem Trace ziehen und den
   recompilierten Code dagegen laufen lassen (nächster Ausbauschritt:
   `arm_trace` um Register-Snapshots an Funktionsgrenzen erweitern).
4. **Frame-Bezug**: `F`-Records erlauben, Coverage und IPC pro Frame zu
   segmentieren (Boot, Menü, Kapitel, Rätsel).
