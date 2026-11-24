# 3DS-Referenz (verifizierte Fakten)

Nur allgemeine, belegte Fakten über die 3DS-Plattform bzw. dieses Spiel-Binary.
Quellen: 3dbrew, libctru, Azahar-Quellcode (`tools/azahar`, nur Referenz).
Keine Experiment-Tagebücher. Neue Einträge nur mit Quelle.

## Speicherlayout (Gast)

| Bereich | Adresse | Bemerkung |
|---|---|---|
| `.text` | `0x00100000` | `code.bin` dekomprimiert, 3.059.712 B |
| Heap (ControlMemory op=3) | `0x08000000` | Spiel fordert `0x5700000` an |
| Linear-Heap | `0x14000000` | Spiel fordert `0x2900000` an (FCRAM linear) |
| GSP-Shared-Memory | `0x10002000` (im Port, 0x1000 B) | InterruptRelayQueue bei `+tid*0x40`, CommandBuffer bei `+0x800`, FB-Update bei `+0x200` |
| VRAM | `0x1F000000` | 6 MiB |
| Stack Hauptthread | endet bei `0x10000000` | |

## IPC

Header in `cmdbuf[0]`: Bits 0..5 `translate_params_size`, 6..11
`normal_params_size`, 16..31 `command_id` (Azahar `core/hle/ipc.h`).

| Deskriptor | Wort 0 | Wort 1 |
|---|---|---|
| CopyHandle | `0x00 \| ((n-1)<<26)` | n Handles |
| MoveHandle | `0x10 \| ((n-1)<<26)` | n Handles |
| CallingPid | `0x20` | PID |
| StaticBuffer | `0x2 \| (id<<10) \| (size<<14)` | Gastadresse |
| MappedBuffer | `0x8 \| (perms<<1) \| (size<<4)` | Gastadresse |

Antwort: `cmdbuf[0]` = Header, `cmdbuf[1]` = Result, danach Parameter.
In `runtime/kernel/hle.c` reserviert `ipc_begin()` das Header-Wort
(`written = 1`); `put_result()` schreibt daher in `cmdbuf[1]`, danach folgen
die Parameter. `normal` zählt Result + Normal-Parameter (ohne Header), wie
Azahar `MakeBuilder(normal, translate)`. `ipc_finish()` leitet den
Header-Wert aus den tatsächlich geschriebenen Wörtern ab, damit Header und
Nutzlast nicht auseinanderlaufen.

Antwort-StaticBuffer: Die Zieladresse steht **nicht** in den
Translate-Parametern des Requests, sondern im Static-Buffer-Bereich hinter dem
Kommandopuffer (`cmd_buf[0x40 + 2*id]` Deskriptor, `+1` Adresse; Azahar
`kernel/hle_ipc.cpp`, `COMMAND_BUFFER_LENGTH = 0x100/4`). Die Antwort schreibt
dorthin und setzt die Adresse in ihren Deskriptor. Beispiel: `dsp::DSP
ReadPipeIfPossible` (Request `normal=3, translate=0`).

### Dienst-Kommandos, die das Spiel benutzt

* **srv:** `1` RegisterClient, `2` EnableNotification, `5` GetServiceHandle.
* **APT:U:** `1` GetLockHandle, `2` Initialize, `3` Enable, `0x0D`
  ReceiveParameter, `0x0E` GlanceParameter, `0x43` InquireNotification,
  `0x4B` AppletUtility, `0x4F` SetAppCpuTimeLimit.
* **fs:USER:** `0x0801` Initialize, `0x0802` OpenFile, `0x0803`
  OpenFileDirectly, `0x080C` OpenArchive, `0x0861` InitializeWithSdkVersion,
  `0x0862` SetPriority.
* **file:** `0x0802` Read, `0x0803` Write, `0x0804` GetSize, `0x0808` Close.
* **gsp::Gpu:** `0x01`–`0x03` WriteHWRegs*, `0x04` ReadHWRegs, `0x05`
  SetBufferSwap, `0x06` SetCommandList, `0x07` RequestDma, `0x0C`
  TriggerCmdReqQueue, `0x10` SetAxiConfigQoSMode, `0x13`
  RegisterInterruptRelayQueue, `0x16` AcquireRight, `0x1E`
  SetInternalPriorities.
* **cfg:u:** `1` GetConfig, `2` GetRegion, `3` GetTransferableId, `5`
  GetSystemModel.
* **ndm:u:** `6` SuspendDaemons, `0x14` OverrideDefaultDaemons.
* **dsp::DSP, ptm:u, y2r:u, boss:U, hid:USER:** siehe Azahar
  `core/hle/service/*`.

Referenzreihenfolge der Dienste im echten Boot (Azahar-Log
`reports/azahar_trace.txt`): APT:U, ndm:u, fs:USER, hid:USER, ptm:u, cfg:u,
gsp::Gpu, y2r:u, dsp::DSP, boss:U.

## cfg (Konfigurationsblöcke)

* `GetConfig(size, block_id)` (cfg:u `0x0001`) schreibt den Block in einen
  Mapped-/StaticBuffer; ein Puffer-Deskriptor mit Adresse 0 bedeutet „nur
  prüfen, nicht schreiben“.
* Wichtige Block-IDs (Azahar `core/hle/service/cfg/cfg.h`): Username
  `0x000A0000` (0x1C, 10 UTF-16-Zeichen + zero + ng_word), Birthday
  `0x000A0001` (2), Language `0x000A0002` (1), CountryInfo `0x000B0000`
  (4: unknown[2], state, country), CountryName `0x000B0001` (16×0x40
  UTF-16), StateName `0x000B0002`, StereoCameraSettings `0x00050005`
  (8 floats), SoundOutputMode `0x00070001` (1), ConsoleModel `0x000F0004`
  (4), EULAVersion `0x000D0000` (4).
* `SystemLanguage`: 0 JP, 1 EN, 2 FR, 3 DE, 4 IT, 5 ES, 6 ZH, 7 KO,
  8 NL, 9 PT, 10 RU, 11 TW.
* Region: 0 JPN, 1 USA, 2 EUR, 3 AUS, 4 CHN, 5 KOR, 6 TWN. Land wird als
  Index in Azahars `country_codes` geliefert (z. B. 110 = „GB“); Region 2
  und Land 110 sind eine gültige EUR/UK-Konsole.
* `GetRegion` (cfg:u `0x0002`, cfg:s `0x0406`, cfg:i `0x0816`) liefert die
  Region aus SecureInfo; auf einer EUR-Konsole ist das 2.
* Azahar-Defaults, angepasst auf eine EUR-Konsole: Sprache EN (1), Land GB
  (110), Modell Old3DS (0), Stereo-Kamera 62/289/76.8/46.08/10/5/55.58/21.57,
  Sound stereo (1), EULA 0x7F7F. (Azahar selbst nutzt für das Modell
  NEW_3DS_XL; der Port bildet eine Old3DS ab.)

## RomFS (SelfNCCH, Archive-ID 3)

IVFC-Header: Magic `IVFC`, Level-Größen als u32 bei `0x10/0x18/0x20`. Die
RomFS-Daten (Level 3) beginnen bei Image-Offset `0x1000`. Das Spiel liest über
`OpenFileDirectly(Archive 3)` ab Stream-Offset 0 den **Level-3**-Header.

Level-3-Header des Spiels (u32-Folge, 0x28 Bytes): header_size `0x28`, dir
hash `0x28/0x24` (9 Buckets), dir meta `0x4C/0x114`, file hash `0x160/0x994`
(613 Buckets), file meta `0xAF4/0xD284`, file data `0xDD80`.

```
Dir : parent+0, sibling+4, childDir+8, childFile+0xC, hashnext+0x10, nameLen+0x14, name@+0x18
File: parent+0, sibling+4, dataOff(u64)+8, dataSize(u64)+0x10, hashnext+0x18, nameLen+0x1C, name@+0x20
```

Kettenende in den Hash-Tabellen = `0xFFFFFFFF`. Hash pro UTF-16-Zeichen:
`h = parent ^ 0x075BCD15; h = (h >> 5 | h << 27) ^ c`, Bucket `h % buckets`
(3dbrew „RomFS“).

Assets: 3 × Level-5-`ARC0` (`lt6_*.fa`), 566 × BCSTM, 34 × MOFLEX. Texturen
`IMGC`, ETC1 (`fmt 0x011B`, 4 bpp, lineare 8×8-Kachelordnung); Dekoder in
`runtime/imgc.c`, Test `tools/test_imgc_c.py`.

## GSP

* Shared-Memory ist 0x1000 Byte groß (Azahar `CreateSharedMemory(..., 0x1000)`),
  im Port bei `0x10002000` gemappt (libctru `svcMapMemoryBlock`).
* InterruptRelayQueue: `+ tid*0x40` (tid = GSP-Client-Index). Index u8 für den
  nächsten Eintrag (absolut, `(index+count) % 0x34`), Anzahl u8, Fehler u8,
  Config u8 (Bit0 = PDC ignorieren), missed PDC0/PDC1 u32, dann `slot[0x34]`.
  IDs: PSC0=0, PSC1=1, **PDC0 (VBlank oben)=2**, PDC1=3, PPF=4, **P3D=5**,
  DMA=6 (Azahar `gsp_interrupt.h`, 3dbrew).
* PDC0/PDC1 gehen an alle registrierten Threads (max. 0x20 Einträge, danach
  nur `missed_*`); andere Interrupts nur an den Thread mit GPU-Recht
  (max. 0x34 Einträge, danach Fehlercode 1). Jeder Eintrag signalisiert das
  bei `RegisterInterruptRelayQueue` übergebene Event.
* Auslöser: PDC0/PDC1 je VBlank (60 Hz), P3D nach ProcessCommandList,
  PSC0/PSC1 nach MemoryFill (nur PSC0, wenn beide Puffer gesetzt sind),
  PPF nach DisplayTransfer/TextureCopy, DMA nach RequestDma.
* CommandBuffer je Thread: `shmem + 0x800 + thread_id * 0x200`. Header:
  Index u8, Anzahl u8, Status u8 (1 = gestoppt, 0x80 = Fehler), ShouldStop u8;
  Kommandos ab `+0x20`, je 0x20 Byte, ID = Wort0 Bits 0..7, Stop = Bit16.
  GX-IDs: 0x00 RequestDma, 0x01 ProcessCommandList, 0x02 MemoryFill,
  0x03 DisplayTransfer, 0x04 TextureCopy, 0x05 FlushCacheRegions.
* DisplayTransfer: Adressen/8 in den Transfer-Registern; Flags Bit0 flip,
  Bit1 input_linear, Bit2 crop_input_lines, Bit5 dont_swizzle, Bits8..10
  bzw. 12..14 Pixelformat, Bits24..25 Scaling. Gekachelt wird in 8x8-Morton-
  Reihenfolge (`VideoCore::GetMortonOffset`).
* Spiele warten im Render-Loop auf PDC0/P3D-Interrupts. Ohne korrekt
  signalisierte Events (RegisterInterruptRelayQueue-Event) hängt der
  Render-Task.

## PICA200 (Kommandostrom)

Paare `(value, header)`. Header: `id` Bits 0..15, `mask` Bits 16..19, `extra`
Bits 20..27, `group` Bit 31. Nach Gruppen-Records mit `extra > 0` folgt bei
ungerader Länge ein Pad-Wort (8-Byte-Ausrichtung).

| Register | Bedeutung |
|---|---|
| `0x080` | Texture-Config, Bit 0 = texture0 enable |
| `0x082` / `0x085` / `0x08E` | Tex0 Größe (h Bits 0..10, w 16..26) / Adresse>>3 / Format |
| `0x200` | Attribut-Puffer-Basis (`addr >> 3`) |
| `0x201`/`0x202` | Attributformate; `0x202` Bits 28..31 = Attributanzahl-1 |
| `0x203 + 3*l` | Loader `l`: Offset, Komponenten, Stride (Bits 16..23) |
| `0x227`–`0x22A` | Index-Puffer, `num_vertices`, Vertex-Offset |
| `0x22E` / `0x22F` | Draw arrays / Draw elements |
| `0x238`–`0x23D` | Kommandopuffer-Kanäle (Größe/Adresse/Jump) |
| `0x2C0`–`0x2FF` | Vertex-Shader (Uniforms, Programm, Swizzle) |
| `0x0C0`–`0x0FF` | TEV-Stufen |

Das Titelbild ist ein 3D-Render (`title.xc`): korrekte Darstellung braucht
einen **Vertex-Shader-Interpreter/Übersetzer** und **TEV**.
`runtime/gpu.c` hat beides noch nicht.

## ARM11-Instruktionssemantik (verifiziert mit Unicorn/Azahar, 2026-11-24)

* **SBC/RSC**: `Rd = Rn - Rm - NOT(C)`, nicht `- C`. Das Ergebnisflag ist
  "kein Borrow" (`Rn >= Rm + cin`). ADC rechnet dagegen `+ C`.
  (Differenztest `tools/recomp_difftest.py`; `arm_dp`.)
* **LSR #32 / ASR #32**: Die Kodierung nutzt `imm5 = 0` als Sonderfall; ein
  Assembler druckt `#32`. Der Shifter muss `amount == 0` (und defensiv
  `>= 32`) als #32 behandeln, sonst wird um 32 Bit (undefiniert) geshiftet.
* **RRX** ist `(C << 31) | (Rm >> 1)`, Carry-Ausgang ist `Rm[0]`. Der
  Shifter-Carry ist reiner Ausgabeparameter; das eingehende C-Flag muss
  separat gelesen werden (`arm_rrx`).
* **PKHBT**: `Rn[15:0] | (Rm LSL shift)[31:16]`; `shift = 0` bedeutet LSL #0.
  **PKHTB**: `Rn[31:16] | (Rm ASR shift)[15:0]`; die Kodierung `imm5 = 0`
  bedeutet ASR #32 (Vorzeichen füllen). Azahar/dyncom approximiert das mit
  ASR #31 (`pkh_inst->imm ? imm : 31`).
* **SXTAB/UXTAB** extendieren 8 Bit, **SXTAH/UXTAH** 16 Bit; optional folgt
  `ROR #8/#16/#24` vor der Extension. SXTB16/UXTB16 rotieren in 8-Bit-Schritten
  (`rot8 * 8`).
* **UBFX/SBFX/BFI/BFC**: Feld `lsb, width`; SBFX sign-extendiert das Feld,
  BFI ersetzt die Feldbits, BFC loescht sie.
* **SSAT/USAT** saettigen den (ggf. geshifteten) 32-Bit-Wert und setzen CPSR.Q
  bei Saettigung; USAT klemmt Negative auf 0.
* **Parallel-/Medienbefehle (ARMv6 SIMD)**:
  - `UADD8/16`: GE = Carry je Lane, `USUB8/16`: GE = kein Borrow je Lane.
  - `SADD8/16`, `SSUB8/16`: GE = 1, wenn die *mathematische* signed Summe bzw.
    Differenz `>= 0` ist (nicht "kein Overflow").
  - Bei 16-Bit-Lanes belegen die GE-Bits Paare: `GE[2i] == GE[2i+1]`.
  - `UHADD8/16` = `(a+b) >> 1`, `UHSUB8/16` = arithmetische Halbierung der
    Differenz (negative Differenz wird nicht auf 0 geklemmt).
  - `QADD/QSUB/QDADD/QDSUB`, `SSAT16/USAT16`, `SMLAxy/SMULxy` setzen CPSR.Q
    bei Saettigung.
* **Thumb-2-IT-Bloecke**: Instruktionen innerhalb eines IT-Blocks sind ohne
  Bedingungsfeld kodiert; der Dekoder muss den ITSTATE
  (`firstcond:mask`, `ITAdvance`: `ITSTATE = (ITSTATE & 0xE0) | ((ITSTATE << 1) & 0x1F)`)
  mitfuehren. Einzeldekodierung ohne den IT-Kontext liefert AL und wuerde die
  Instruktion unbedingt ausfuehren. `it/itt/ite/...` selbst ist ein No-op.
  (Recompiler: `emit.it_conditions`, gesetzt in `cli.generate`.)
* **capstone 5.0.7** setzt `update_flags` fuer `adc/sbc/rsc` immer auf True
  (C-Flag als impliziter Eingang). Das S-Bit muss aus der Kodierung gelesen
  werden: ARM Bit 20, 32-Bit-Thumb Bit 4 des ersten Halfwords.
* **Unicorn 2.1.4 (Testwerkzeug)**: Thumb wird nur ueber Bit 0 der
  Startadresse gewaehlt (`emu_start(addr | 1)`); `UC_MODE_THUMB` allein und
  CPSR.T genuegen nicht.
* **ADR in Thumb** ist PC-relativ mit `Align(PC, 4)` (PC = addr + 4); im
  Recompiler bereits so umgesetzt.

* **16-Bit-Thumb im IT-Block**: Die "S"-Formen (ADD/SUB/AND/EOR/LSL/LSR/
  ASR/ADC/SBC/ROR/RSB/ORR/MUL/MOV/MVN/BIC, T1) setzen innerhalb eines
  IT-Blocks **keine** Flags (ARM ARM A7.7.3); TST/CMP/CMN dagegen immer.
  Die Kodierung sagt das nicht, der Recompiler braucht den ITSTATE.
  (Differenztest `--mode cf`, Fall `thumb_it_*`; `emit.IT_NO_FLAGS_16`.)
* **QDADD/QDSUB**: verdoppelt wird der **zweite** Quelloperand
  (`QDADD Rd,Rm,Rn`: `Rd = sat(Rm + sat(2*Rn))`, `QDSUB` analog `-`), und die
  Verdopplung saturiert selbst (setzt Q). Belegt mit Unicorn und Azahar
  `dyncom QDADD_INST`.
* **SMLAxy** setzt bei Ueberlauf der Addition nur Q; das Ergebnis ist die
  untere 32-Bit-Summe (kein Saettigen, Azahar `handle_Smlaxy`).
* **CBZ/CBNZ**: Operand 0 ist das Register, Operand 1 das Ziel (Capstone wie
  Assembler); der Sprung ist nur **vorwaerts** und in Halbwort-Schritten
  kodiert (Rueckwaertsschleife braucht ein separates `b`).
* **PC-Schreibweisen** im Recompiler: `bx lr`/`mov pc,lr`/`pop {..,pc}`/
  `ldm sp!,{..,pc}` sind Rueckkehr (Rueckkehr zum C-Aufrufer); `pop`/`ldm`
  mit PC setzen das Interworking-Bit. `bx rN`, `blx rN`, `ldr pc,[..]`,
  `add pc,pc,rN,lsl #2` und Thumb `tbb/tbh` springen dynamisch: Ziele
  innerhalb der Funktion per `switch`/`goto` (auch mitten in einem Block,
  dafuer Labels an jeder Instruktion), sonst `arm_call_guest`.
* **Sprungtabellen**: Bei `add pc,pc,rN,lsl #2` folgt ab `pc+8` eine Tabelle
  aus 4-Byte-`b`-Instruktionen; bei `ldr pc,[pc,rN,lsl #2]` stehen dort
  absolute Codezeiger (ARM `pc+8`, Thumb `Align(pc+4,4)`); `tbb/tbh` nutzen
  Byte-/Halbwort-Offsets ab `Align(pc+4,4)`. Der Scanner legt diese Ziele als
  Bloecke an (`scan.ldr_pc_table`/`tbb_table`).

## Quellen
* https://www.3dbrew.org/wiki/IPC, /RomFS, /GSP_Shared_Memory, /GPU/Internal_Registers
* libctru: `source/services/{apt,gsp,srv,fs}.c`, `source/ndsp/ndsp.c`
* Azahar: `src/core/hle/service/**`, `src/video_core/pica/**`
