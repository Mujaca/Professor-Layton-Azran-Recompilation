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
  `GetServiceHandle` erhält den Namen als 8-Byte-Feld plus `name_len` =
  `strlen(name)` (nicht `strlen+1`); `name_len > 8` → `0xD9006405`
  (Azahar `sm/srv.cpp`, `kMaxPortSize = 8`).
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
Bits 20..27, `group` Bit 31. Jeder Record beginnt 8-Byte-ausgerichtet; nach
einem Record mit ungerader Wortzahl folgt ein Pad-Wort. `extra > 0`:
`group=1` schreibt die Extra-Woerter nach `id+1..id+extra` (sequentielle
Register), `group=0` schreibt `id` selbst `extra`-mal (Shader-/LUT-Upload).
Die Maske wird byteweise angewandt (`Azahar ExpandBitsToBytes`, z. B. `0x5`
= Bytes 0 und 2). Quelle: `pica_core.cpp` `ProcessCmdList`/`WriteInternalReg`.

| Register | Bedeutung |
|---|---|
| `0x010` | `irq_request` (Finalize): bei Byte-Treffer gegen `irq_compare` (`0x020`) P3D-Interrupt; `irq_autostop` (`0x034`, Default 1) beendet die Liste. GSP-Defaults: `irq_compare=0x12345678`, `irq_mask=0xFFFFFFF0`, `irq_autostop=1`. |
| `0x080` | `main_config`: Bit0..2 Textur 0..2 enable, Bit10 Textur 3, Bit13 `texture2_use_coord1`, Bit16 `clear_texture_cache` |
| `0x081`/`0x091`/`0x099` | Textureinheit 0/1/2: `+0` border_color, `+1` Größe (h Bits 0..10, w 16..26), `+2` Wrap (`wrap_t` Bits 8..10, `wrap_s` 12..14), Filter (mag Bit 1, min Bit 2, mip Bit 24), Typ Bits 28..30 (0 = 2D, 5 = disabled), `+3` LOD, `+4` Adresse>>3. **Nur Einheit 0 beachtet den Typ** (3dbrew; Azahar `TextureColor`); Einheit 3 ist die prozedurale Textur (0xA8..0xB7), kein adressiertes Bild. |
| `0x08E`/`0x096`/`0x09E` | Texturformat 0..13: RGBA8, RGB8, RGB5A1, RGB565, RGBA4, IA8, RG8 (HILO8), I8, A8, IA4, I4, A4, ETC1, ETC1A4 |
| `0x200` | Attribut-Puffer-Basis (physische Adresse = `(reg>>1)*16`) |
| `0x201`/`0x202` | Attributformate; `0x202` Bits 28..31 = Attributanzahl-1 |
| `0x203 + 3*l` | Loader `l`: Offset, Komponenten, Stride (Bits 16..23) |
| `0x227`–`0x22A` | Index-Puffer, `num_vertices`, Vertex-Offset |
| `0x22E` / `0x22F` | Draw arrays / Draw elements |
| `0x238`/`0x23A`/`0x23C` | Kanal 0: CMDBUF_SIZE (Bytes/8, Bits 0..19) / CMDBUF_ADDR (physisch, /8) / CMDBUF_JUMP (Trigger) |
| `0x239`/`0x23B`/`0x23D` | Kanal 1: dieselben Felder |
| `0x2C0`–`0x2FF` | Vertex-Shader (Uniforms, Programm, Swizzle) |
| `0x0C0`–`0x0FF` | TEV-Stufen |
| `0x0A8`–`0x0AD` | Prozedurale Textur (Einheit 3): Konfiguration, Noise, LUT-Parameter |
| `0x0AF` | Proctex-LUT-Index (Bits0..7) und Tabelle (Bits8..11) |
| `0x0B0`–`0x0B7` | Proctex-LUT-Daten; jedes Wort schreibt bei `index` und erhöht ihn |

Ein Schreibzugriff auf `0x23C`/`0x23D` startet die Liste des Kanals und
**ersetzt** die laufende Liste (Azahar `cmd_list.Reset`): der Rest der alten
Liste wird nicht mehr ausgeführt. Ungültige Adresse/Größe meldet P3D.

### PICA-Adressen

PICA-Register (Textur `0x085`, Vertex-Basis `0x200`, CMDBUF_ADDR, im
Kommandostrom geschriebene Pufferadressen) enthalten **physische** Adressen.
Der Port bildet sie auf seinen linearen Gast-Speicher ab
(`runtime/gpu/gpu.c` `gpu_pica_phys_to_guest`, Azahar `memory.h`):

| physisch | Gast-linear |
|---|---|
| `0x18000000`–`0x185FFFFF` (VRAM) | `0x1F000000`–`0x1F5FFFFF` |
| `0x20000000`–`0x27FFFFFF` (FCRAM) | `0x14000000`–`0x1BFFFFFF` (Linear-Heap) |

GSP-Submits liefern dagegen Virtualadressen; `GPU::Execute` wandelt sie vor
dem Eintrag in die Kommandopuffer-Register um (`gpu_pica_virt_to_phys`).
Bereiche außerhalb dieser Fenster bleiben unverändert, weil der Port keine
MMU/Seitentabelle hat.

### Vertex-Pipeline

* Shader-Register: `vs` 0x2B0..0x2DF (`gs` 0x280..0x2AF). `bool_uniforms`
  0x2B0 (Bit je Uniform), `int_uniforms` 0x2B1..0x2B4, Einstiegspunkt
  **0x2BA** `main_offset` Bits 0..15 (Programmstart, nicht pc=0),
  `uniform_setup.index`/Format 0x2C0 (Bit31 = float32), `set_value`
  0x2C1..0x2C8, `program.offset` 0x2CB + `set_word` 0x2CC..0x2D3,
  `swizzle.offset` 0x2D5 + `set_word` 0x2D6..0x2DD. Der Uniform-Index wird
  je vollständigem Vec4 um eins erhöht (7 Bit); Indizes ≥ 96 werden ignoriert.
* float24-Packing (3 Wörter je Vec4): `x=w2&0xFFFFFF`,
  `y=((w1&0xFFFF)<<8)|(w2>>24)`, `z=((w0&0xFF)<<16)|(w1>>16)`, `w=w0>>8`;
  float24 = Sign Bit23, Exponent Bits16..22 (Bias 63), Mantisse Bits0..15.
  float32-Modus: 4 Wörter, Komponente `3-i` aus Wort `i`.
* Attribute: 0x200 Basis (physisch, `(reg>>1)*16`), 0x201/0x202 Formate
  (0..3 = BYTE/UBYTE/SHORT/FLOAT, Bits 2..3 = Komponenten-1), 0x202
  Bits16..27 `attribute_mask` (gesetzt = Default-Attribut), Bits28..31
  Maximalindex. Loader `0x203+3*l`: `data_offset`, 12 Komponentennibbles,
  `byte_count`/Stride, `component_count`; der Offset wird vor jeder
  Komponente auf ihre Elementgröße ausgerichtet, Attribute 12..15 sind
  4/8/12/16-Byte-Padding. Default-Attribute: 0x232 Index,
  0x233..0x235 `set_value` (gepackte float24-Queue); Index 0xF = Immediate.
* Index-Puffer 0x227: Offset relativ zur Attributbasis, Bit31 = u16.
  0x22E = DrawArrays (nutzt `vertex_offset` 0x22A), 0x22F = DrawElements
  (Index-Puffer, `vertex_offset` wird ignoriert).
* Ausgang: `output_mask` 0x2BD wählt o0..o15; 0x04F `vs_output_total`,
  0x050..0x056 `vs_output_attributes` (5-Bit-Semantik je Komponente:
  POSITION 0..3, COLOR 8..11, TEXCOORD0 12/13, TEXCOORD1 14/15,
  TEXCOORD0_W 16, VIEW 18..20, TEXCOORD2 22/23). Nicht gemappte Semantiken
  sind 1.0; Farben werden vor der Interpolation abs/saturiert.
* Instruktionen sind ein Wort: Opcode Bits26..31, Swizzle-Muster je
  `operand_desc_id`. Bedingungen vergleichen `refx`/`refy` mit
  `conditional_code` (Or/And/JustX/JustY). `LOOP` nutzt ein Int-Uniform
  (x = Downcounter, y = aL-Start, z = aL-Inkrement); aL ist
  `address_register[2]`. Bei nicht-invertierten Befehlen ist `src1` das
  7-Bit-Register (Uniform/adressierbar), `src2` nur 5 Bit (Input/Temp).
* **SrcInversed** (nihstro `OpCode::Info::SrcInversed`): `DPHI 0x18`,
  `DSTI 0x19`, `SGEI 0x1A`, `SLTI 0x1B` und `MADI 0x30..0x37`. Bei den
  Common-Befehlen 0x18..0x1B ist `src1` 5 Bit (`src1i` Bits14..18) und
  `src2` 7 Bit (`src2i` Bits7..13); MADI hat `src1` 5, `src2` 5 und `src3`
  7 Bit (`src3i` Bits5..11). Das Adressregister wirkt auf die 7-Bit-Quelle
  (common: `src2`; MAD normal: `src2`; MADI: `src3`).
* Opcode-Formate (Quelle: nihstro `shader_bytecode.h`, Azahar
  `shader_interpreter.cpp`/`shader_jit_x64_compiler.cpp`):

  | Opcodes | Name | Operanden |
  |---|---|---|
  | `0x00` `0x01` `0x02` `0x03` | ADD, DP3, DP4, DPH | 2 (DP3 nur xyz, DPH/DPHI setzen `src1.w=1`) |
  | `0x04` `0x08` `0x09` `0x0A` `0x0C` `0x0D` | DST, MUL, SGE, SLT, MAX, MIN | 2 |
  | `0x05` `0x06` `0x07` `0x0B` `0x0E` `0x0F` `0x13` | EX2, LG2, LIT, FLR, RCP, RSQ, MOV | 1 (`exp`/`log`/`rcp`/`rsq` lesen nur `src1.x`) |
  | `0x12` | MOVA | 1, schreibt `a0.x/y` nach Dest-Maske (kein Dest-Register) |
  | `0x18` `0x19` `0x1A` `0x1B` | DPHI, DSTI, SGEI, SLTI | 2, invertiert |
  | `0x20` `0x21` `0x22` `0x2A` `0x2B` | BREAK, NOP, END, EMIT, SETEMIT | 0 (trivial; EMIT/SETEMIT nur GS) |
  | `0x23` `0x2C` `0x2D` | BREAKC, JMPC, JMPU | Flow, `num`/`dest_offset`/Bedingung |
  | `0x24` `0x25` `0x26` `0x27` `0x28` `0x29` | CALL, CALLC, CALLU, IFU, IFC, LOOP | Flow |
  | `0x2E` `0x2F` | CMP | 2, Opcode-LSB ignoriert, schreibt `conditional_code` |
  | `0x30..0x37` | MADI | 3, invertiert (src3 7 Bit) |
  | `0x38..0x3F` | MAD | 3 (src2 7 Bit), untere 3 Opcode-Bits ignoriert |

  `DST`/`LIT` sind definiert, werden aber auch von Azahar nicht ausgeführt
  (Interpreter meldet „Unhandled instruction“, kein Register-Schreibzugriff).
* Common-Format: `opcode` 26..31, `operand_desc_id` 0..6, `src2` 7..11,
  `src1` 12..18, `address_register_index` 19..20, `dest` 21..25. Invertiert:
  `src2i` 7..13, `src1i` 14..18. MAD: `operand_desc_id` 0..4, `src3` 5..9,
  `src2` 10..16, `src1` 17..21, `address_register_index` 22..23, `dest`
  24..28; invertiert `src3i` 5..11, `src2i` 12..16. Flow-Format:
  `num` 0..7, `dest_offset` 10..21, `op` 22..23 bzw.
  `bool_uniform_id`/`int_uniform_id` 22..25, `refy` 24, `refx` 25.

Das Titelbild ist ein 3D-Render (`title.xc`): korrekte Darstellung braucht
zusätzlich **TEV** und die Rasterizer-Zustände (Viewport/Clip/Depth).

### Geometrie-Shader (Shader-Einheit 3)

Quelle: Azahar `pica/regs_shader.h`, `pica/regs_pipeline.h`,
`pica/geometry_pipeline.cpp`, `pica/shader_unit.cpp` (`GeometryEmitter`),
`pica/pica_core.cpp` (`HandleSpecialReg`), nihstro `shader_bytecode.h`.

* Register `0x280..0x2AF` sind eine zweite Shader-Einheit mit demselben
  Aufbau wie der VS (`0x2B0..0x2DF`, Versatz -0x30): `0x280`
  `bool_uniforms`, `0x281..0x284` `int_uniforms`, `0x289`
  `input_buffer_config` (Bits 0..3 `max_input_attribute_index`, 8..15
  `input_to_uniform`, 24..31 `shader_mode` 8 = GS / 0xA0 = VS), `0x28A`
  `main_offset`, `0x28B`/`0x28C` Input-Map (4 Bit je Attribut), `0x28D`
  `output_mask`, `0x290` Uniform-Index (Bit31 = float32), `0x291..0x298`
  `set_value`, `0x29B` Programm-Offset, `0x29C..0x2A3` `set_word`, `0x2A5`
  Swizzle-Offset, `0x2A6..0x2AD` `set_word`. Programm- und Swizzle-RAM sind je
  4096 Wörter groß; die **VS**-Register adressieren nur Wort 0..511
  (Schreibzugriffe darüber werden verworfen), die GS-Register alle 4096.
* Pipeline: `0x229` Bits 0..1 `use_gs` (2 = an), Bit31 `variable_primitive`;
  `0x242` `max_input_attribute_index` des Vertex-Loaders/Immediate-Modus;
  `0x244` `gs_unit_exclusive_configuration` (0: VS-Uploads spiegeln Programm,
  Swizzle, Uniforms, Bool-/Int-Uniforms und `output_mask` nach Einheit 3,
  solange `use_gs == 0`); `0x24A`/`0x251` `vs_outmap_total_minus_1_a/b`
  (Anzahl VS-Ausgangsattribute); `0x252` `gs_config` (Bits 0..7 Modus
  0 Point / 1 VariablePrimitive / 2 FixedPrimitive, 8..11
  `fixed_vertex_num_minus_1`, 12..15 `stride_minus_1`, 16..23 `start_index`);
  `0x254` `variable_vertex_main_num_minus_1`.
* **Point:** VS-Ausgaben füllen den GS-Eingangspuffer (`0x289` Bits 0..3 + 1
  Attribute); ist er voll, läuft der GS mit Input-Map `0x28B`/`0x28C`.
  **VariablePrimitive:** der erste Wert eines Index-Batches ist die
  Vertexzahl und steht danach als Uniform `f[0]`; die Vertexdaten landen ab
  `f[1]` in den Uniforms, nur die ersten `0x254+1` Vertices erhalten alle
  Attribute, alle weiteren nur Attribut 0. **FixedPrimitive:** wie Variable,
  aber feste Vertexzahl ab `start_index`. Ohne GS geht die VS-Ausgabe direkt
  in die Primitive Assembly.
* `EMIT` (0x2A) kopiert die Ausgangsregister laut `output_mask` nach
  `buffer[vertex_id]` und wechselt danach die Ausgangsbank. Es gibt **zwei**
  Bänke: ein zwischen zwei EMITs nicht beschriebenes Register liefert den
  Wert von vor zwei EMITs. Bei `prim_emit` gehen alle drei Puffer in die
  Primitive Assembly; `winding` invertiert dort (nur Topologie Shader)
  die Windung des nächsten Dreiecks. `SETEMIT` (0x2B): Bits 0..1
  `vertex_id`, Bit 2 `prim_emit`, Bit 3 `winding`. Bool-Uniform b15 ist nach
  jedem GS-Aufruf gesetzt.
* `ConfigOutput` kopiert `output_mask` (0x28D) beim `Reconfigure` (je Draw)
  in den Emitter; spätere Schreibzugriffe auf 0x28D wirken erst beim nächsten
  Draw. Der Immediate-Modus (`0x232 == 0xF`, Attribute über `0x233..0x235`)
  läuft ebenfalls durch die Geometrie-Pipeline (`PicaCore::DrawImmediate`);
  `immediate.Reset()` beim Schreiben von 0x232 setzt
  `reset_geometry_pipeline`, sodass erst der nächste Immediate-Flush
  rekonfiguriert und der Backend-Zustand über weitere Vertices erhalten
  bleibt.

### Rasterizer, Clipping, TEV und Farbziel

Quelle: Azahar `renderer_software/sw_rasterizer.cpp`, `sw_clipper.cpp`,
`sw_framebuffer.cpp`, `sw_texturing.cpp`, `pica/regs_*.h`.

* **Clip-Raum:** Position ist `(x,y,z,w)`; sichtbar ist `-w <= x,y <= w`,
  `-w <= z <= 0`, `w > 0` (PICA-Eigenheit: z zeigt nach hinten, Azahar
  setzt im GLSL-Shader `gl_Position.z = -z`). Zusätzlich optional eine eigene Ebene
  (`clip_enable` 0x47, Koeffizienten `clip_coef[4]` 0x48..0x4B). Clipping ist
  Sutherland-Hodgman gegen diese 7(+1) Ebenen; ein Dreieck wird zu einem
  Polygon mit bis zu 3+7 Ecken und danach als Fan trianguliert.
* **Viewport:** `0x41`/`0x43` sind halbe Breite/Höhe als float24, `0x68`
  Bits 0..9 = X- und Bits 16..25 = Y-Ecke (signed). Bildschirmkoordinate
  `x = (x/w + 1) * half_w + corner_x`. `0x4D`/`0x4E` sind
  `viewport_depth_range`/`viewport_depth_near_plane` (float24).
* **Perspektivkorrektur:** Attribute werden vor der Division mit `1/w`
  multipliziert. Im Bildschirmraum werden `attr/w` und `1/w` linear
  (baryzentrisch) interpoliert und danach durcheinander geteilt, also
  `attr = sum(attr_i/w_i * A_i) / sum(1/w_i * A_i)`.
* **Coverage:** Rasterizer-Koordinaten sind 12.4-Fixed-Point (Pixel*16).
  Baryzentrische Gewichte sind ganzzahlige signed areas; die Kante
  „rechts oder flach unten“ bekommt Bias -1 (Top-Left-Regel). Scissor
  0x65 (Modus Bits0..1, x1/y1) / 0x66 (x2/y2) schneidet das Bounding-Box.
  Cull-Modus 0x40 Bits 0..1: 0/3 = alle (Dreieck wird auf CCW gedreht),
  1 = nur im Uhrzeigersinn, 2 = nur gegen den Uhrzeigersinn.
* **Primitive Assembly:** Topologie 0x25E Bits 8..9 (0 Liste, 1 Strip,
  2 Fan, 3 Shader). Strip/Fan halten zwei Vertices über Draws hinweg; bei
  Strip wird jedes zweite Dreieck als `(v2,v1,v3)` ausgegeben, damit beide
  dieselbe Windung haben. 0x25F `restart_primitive` setzt den Zustand
  zurück, ebenso ein Topologiewechsel.
* **TEV (6 Stufen):** Basis 0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8; je Stufe
  `+0` Quellen, `+1` Modifier, `+2` Operationen, `+3` `const_color`,
  `+4` Skalierung. Quellen: 0 PrimaryColor (interpolierte Vertexfarbe),
  1/2 Primary/Secondary-Fragmentfarbe (Lighting), 3..6 Texture0..3,
  0xD PreviousBuffer, 0xE Constant, 0xF Previous. In Stufe 0 wird
  `Previous` durch Quelle 3 ersetzt. Kombinierer: Replace/Modulate/Add/
  AddSigned/Lerp/Subtract/Dot3/MultiplyThenAdd/AddThenMultiply; Skalierung
  = `1 << scale` (Wert 3 = 1). Combiner-Buffer: 0xE0 Bits 8..11 (RGB) /
  12..15 (Alpha) markieren Stufen 0..3, die den Puffer aktualisieren;
  0xFD ist die Puffer-Farbe.
* **Texturen:** Jede 8x8-Kachel liegt Morton-verschachtelt
  (`MortonInterleave(x,y)`, xlut/ylut-Tabellen wie Azahar `VideoCore`), die
  Kacheln selbst zeilenweise, die Textur von unten nach oben. Bytes je Kachel:
  RGBA8 256, RGB8 192, 16-Bit-Formate 128, 8-Bit 64, I4/A4 32, ETC1 32,
  ETC1A4 64. Kanalreihenfolge wie in Azahar `common/color.h` (Speicher =
  aufsteigende Bytes): RGBA8 `a,b,g,r` (32-Bit-Wort `0xRRGGBBAA`), RGB8
  `b,g,r`, IA8 `a,i`, RG8 `g,r`, IA4 `iiii aaaa`, I4/A4 zwei Nibbles je Byte
  (ungerader Morton-Index = oberes Nibble). ETC1 unterteilt jede Kachel in
  vier 4x4-Subtiles; die Subtile ist `x/4 + 2*(y/4)` aus den rohen
  Kachelkoordinaten (nicht aus dem Morton-Index), der Alpha-Wert von ETC1A4
  liegt bei Shift `4*((x%4)*4 + y%4)`. Differential-/Individual-Modus,
  `table_subindexes` Bits 0..15, `negation_flags` 16..31, flip Bit 32.
  Adresse = Reg*8 physisch.
* **Sampler:** `s = trunc(u * width)`, `t = trunc(v * height)`; Wrap-Modi
  0 ClampToEdge, 1 ClampToBorder (border_color), 2 Repeat, 3 MirroredRepeat,
  4 ClampToEdge2 (negative Koordinate = Repeat), 5 ClampToBorder2 (positiv
  außerhalb = Border), 6/7 wie Repeat. Erst wrappen, dann
  `t = height - 1 - t`. Der Azahar-Software-Renderer wertet Min-/Mag-Filter
  nicht aus (`TODO: Apply the min and mag filters`), sondern liest immer den
  nächsten Texel.
* **Prozedurale Textur (Einheit 3, `sw_proctex.cpp`):** eigener Zustand in
  0x0A8..0x0AD/0x0AF/0xB0..0xB7, kein adressiertes Bild. 0x0A8: `u_clamp`
  Bits0..2, `v_clamp` 3..5, `color_combiner` 6..9, `alpha_combiner` 10..13,
  `separate_alpha` Bit14, `noise_enable` Bit15, `u_shift` 16..17,
  `v_shift` 18..19. 0x0A9/0x0AA: Amplitude (fix1.3.12, Bits0..15 signed)
  und Phase (float16, Bits16..31). 0x0AB: Frequenz u/v je float16.
  0x0AC: Filter Bits0..2, `width` Bits11..18. 0x0AD: LUT-Offset `level0`
  Bits0..7. 0x0AF: `index` Bits0..7, `ref_table` Bits8..11 (0 Noise,
  2 ColorMap, 3 AlphaMap, 4 Color, 5 ColorDiff); 0xB0..0xB7 schreiben je
  ein Wort bei `index` und erhöhen ihn (mod 256; 128er-Tabellen modulo 128).
  Wertetabellen: 128 Einträge, Bits0..11 value/4095, Bits12..23 signed
  diff/4095. Farbtabellen: 256 × RGBA8; ColorDiff je Kanal signed Byte ×2.
  Ablauf: `u=|u|`, `v=|v|`; Shift-Offset `0.5` (`1.0` bei MirroredRepeat)
  aus `((int)v/2)%2` (Odd) bzw. `(((int)v+1)/2)%2` (Even); optional Rauschen
  (Rand1D/Rand2D-Tabellen, bilinear mit der Noise-LUT); Shift, Clamp
  (0 ToZero, 1 ToEdge, 2 SymmetricalRepeat, 3 MirroredRepeat, 4 Pulse),
  Combiner (U, U², V, V², (u+v)/2, (u²+v²)/2, sqrt(u²+v²), Min, Max,
  RMax) durch die ColorMap-LUT; Farbe = ColorTable[offset + c*(width-1)]
  (Nearest oder Linear mit Diff-Tabelle). `separate_alpha` ersetzt Alpha
  durch die AlphaMap desselben Combiners. Die Min-/Mag-Filter der Textur-
  einheit wertet Azahar nicht aus; Mipmap-LOD ist dort ebenfalls TODO, die
  Mipmap-Filter verhalten sich wie Nearest/Linear.
* **Texturtypen (nur Einheit 0, 3dbrew/Azahar `TextureColor`):** Typ in
  0x83 Bits28..30: 0 Texture2D, 1 TextureCube, 2 Shadow2D, 3 Projection2D,
  4 ShadowCube, 5 Disabled (liefert 0,0,0,0). Einheiten 1/2 ignorieren den
  Typ. `Projection2D` teilt u/v durch `tc0_w` (Semantik 16, TEXCOORD0_W).
  `Cube`/`ShadowCube` wählen die dominante Achse von (u,v,tc0_w), Face-Adresse
  `address*8` mit Bits0..21 aus `0x86..0x8A` (übrige Bits von 0x85),
  `u'=x/z*0.5+0.5`, `v'=y/z*0.5+0.5` (Division ohne Sonderfall; z==0
  ergibt inf/NaN wie in Azahar `ConvertCubeCoord`). `Shadow2D` teilt bei
  `0x8B` Bit0 = 0 (perspektivisch) u/v durch tc0_w; Shadow-Tiefe ist
  `|tc0_w|` (Cube: `|z|`). Danach je Texel
  `z_int = min(shadow_z,1)*0xFFFFFF - (bias<<1)` (bias = 0x8B Bits1..23) und
  `z_ref = (a<<16)|(b<<8)|g`; Density = r wenn `z_ref >= z_int`, sonst 0
  (alle vier Kanäle).
* **Fragment-Lighting (`0x140..0x1D9`, Azahar `sw_lighting.cpp`,
  `regs_lighting.h`):** 8 Lichtquellen à 0x10 Wörter ab 0x140 (specular_0,
  specular_1, diffuse, ambient als 10-Bit-Kanäle/255; Position x/y/z als f16;
  Spot-Richtung x/y/z als fix1.1.11/2047; config Bits0..3 directional/
  two_sided/geometric0/1; dist_atten_bias/scale als f20 = Float<12,7>).
  `0x1C0` global_ambient, `0x1C2` max_light_index, `0x1C3` config0
  (Bit0 Shadow, Bits2/3 Primary/Secondary-Alpha, Bits4..7 Config 0..8,
  Bits16/17 Shadow primary/secondary, Bit18 invert, Bit19 Shadow-Alpha,
  Bits22..23 Bump-Selector, Bits24..25 Shadow-Selector, Bit27 Clamp,
  Bits28..29 Bump-Modus, Bit30 Renorm aus), `0x1C4` config1
  (Bits0..7 Shadow aus, 8..15 Spot aus, 16/17 D0/D1-LUT aus, 19 Fresnel,
  20..22 Reflect R/G/B, 24..31 Distanz aus), `0x1C5` LUT-Typ (Bits8..12)
  und -Index (Bits0..7), `0x1C6` Bit0 Lighting aus, `0x1C8..0x1CF` LUT-Daten
  (jedes Wort landet bei Typ/Index, danach Index+1 mod 256),
  `0x1D0` Abs-Modus je Eingang, `0x1D1` LUT-Eingang je Kanal
  (0 NH, 1 VH, 2 NV, 3 LN, 4 SP, 5 CP), `0x1D2` Skalierung
  (0 1x, 1 2x, 2 4x, 3 8x, 6 0.25x, 7 0.5x), `0x1D9` Light-Enable
  (3 Bit je Slot). 24 LUTs à 256 Einträge: Bits0..11 `value`/4095,
  Bits12..22 `difference`/2047, Bit23 negatives Vorzeichen. Ohne Abs-Modus
  ist der Index signed (result*128), sonst unsigned (result*256). Im
  Abs-Modus wird nur bei gesetztem `two_sided_diffuse` (LightSrc.config
  Bit1) der Betrag genommen, sonst auf 0 geklemmt (`max(result,0)`).
  Bump-Modus 3 ist undefiniert: Normal und Tangente bleiben 0 (Azahar
  `LOG_ERROR`). Entgegengesetzte Vertex-Quaternionen werden vor Clipping
  und Interpolation an Vertex 0 angeglichen (`FlipQuaternionIfOpposite`:
  Skalarprodukt < 0 ⇒ alle vier Komponenten negieren).
  Ergebnis (Diffus+Global-Ambient, Specular) wird 0..1 geklemmt und als
  Primary/Secondary-Fragment den TEV-Quellen 1/2 zugeführt.
* **Farbziel:** Output-Merger 0x100..0x107: 0x100 Bit0..1
  `fragment_operation_mode`, Bit8 `alphablend_enable`; 0x101 Gleichungen
  (RGB Bits 0..2, Alpha 8..10) und Faktoren (Src RGB 16..19, Dst RGB 20..23,
  Src A 24..27, Dst A 28..31); 0x102 LogicOp Bits 0..3; 0x103 `blend_const`;
  0x104 Alpha-Test (Bit0 enable, Funktion Bits 4..6, Referenz Bits 8..15);
  0x105 Stencil (enable, Funktion, write_mask, reference, input_mask),
  0x106 Stencil-Aktionen (stencil_fail/depth_fail/depth_pass); 0x107
  `depth_test_enable` Bit0, `depth_test_func` Bits 4..6, Kanal-Masken
  Bits 8..11, `depth_write_enable` Bit 12. Faktoren 0 Zero, 1 One,
  2/3 Source, 4/5 Dest, 6/7 SourceAlpha, 8/9 DestAlpha, 10/11 ConstantColor,
  12/13 ConstantAlpha, 14 SourceAlphaSaturate. Gleichungen Add/Subtract/
  ReverseSubtract/Min/Max, Ergebnis `(s*sf ± d*df) / 255`.
* **Framebuffer 0x110..0x11E:** `allow_color_write` 0x113,
  `allow_depth_stencil_write` 0x115, `depth_format` 0x116 Bits 0..1
  (D16/D24/D24S8), `color_format` **0x117** Bits 16..18 (RGBA8/RGB8/RGB5A1/
  RGB565/RGBA4), `block32` 0x11B, Tiefenpuffer **0x11C**, `color_buffer_address`
  **0x11D** (physisch = Wert*8), **0x11E** Breite Bits 0..10, Höhe als Feld
  Bits 12..21 (= tatsächliche Höhe - 1, Zugriff `+1`), Flip Bit 24. Pixel
  liegen 8x8-Morton-gekachelt und von unten nach oben (`y' = height_raw - y`,
  Offset = `morton(x,y') + (y' & ~7) * width * bpp`); Tiefe/Stencil nutzen
  dieselbe Kachelung mit 2/3/4 Byte je Pixel, D24/D24S8 little endian,
  Stencil im vierten Byte.
* **Tiefe:** `depth = interpoliert(z/w) * viewport_depth_range (0x4D) +
  viewport_depth_near_plane (0x4E)`. Bei `depthmap_enable` 0x6D = 0
  (W-Buffer) wird zusätzlich mit `w` skaliert. `z_int =
  depth * (2^Bits - 1)`; Schreibzugriff nur bei `depth_write_enable` und
  erlaubtem Schreiben. Stencil-Test greift nur bei D24S8.
* **Fog:** `fog_mode` 0xE0 Bits0..2 (0 aus, 5 Fog, 7 Gas), `fog_flip`
  Bit16, Farbe 0xE1 r/g/b. Die 128 LUT-Einträge werden über 0xE8..0xEF
  geladen: jedes Wort landet bei `fog_lut_offset` (0xE6) % 128, danach
  inkrementiert die Hardware den Offset. Eintrag = Bits13..23 `value`
  (0.0.11) und Bits0..12 `difference` (1.1.11), jeweils /2047.
  `fog_index = (flip ? 1-depth : depth) * 128`; Faktor = `value +
  difference * (fog_index - floor(fog_index))`, geklemmt auf 0..1; Ergebnis
  `fog*farbe + (1-fog)*fog_color` auf RGB. Reihenfolge im Pixel:
  TEV → Alpha-Test → Fog → Depth/Stencil → Blending (Azahar
  `sw_rasterizer.cpp`).
* **Texturkoordinaten:** `t` wird vor dem Sample invertiert
  (`t = height-1 - wrapped(t)`), da Texturen von unten nach oben liegen.

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
* **ARM/Thumb-Interworking-Stubs**: `add rN, pc, #imm` (ARM PC = addr + 8,
  `imm` enthält das Thumb-Bit, z. B. `0x292264: add r12, pc, #1; bx r12` ->
  `0x29226C`) bzw. `adr rN, #imm` + `bx rN`. Ebenso `ldr pc, [pc, #imm]` mit
  einem Literal `ziel|1` (`0x24CCEC: ldr pc,[pc,#-4]` -> `0x100373`). Die
  Ziele sind eigenständige Thumb-Code-Einstiege (auch mitten in einer anderen
  ARM-Funktion) und müssen als Thumb-Funktion geseedet werden; sonst findet
  `arm_call_guest` sie nicht und der Aufruf wird ein No-op.

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
  `GetServiceHandle` erhält den Namen als 8-Byte-Feld plus `name_len` =
  `strlen(name)` (nicht `strlen+1`); `name_len > 8` → `0xD9006405`
  (Azahar `sm/srv.cpp`, `kMaxPortSize = 8`).
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

fs-Result-Codes (Azahar `core/file_sys/errors.h`): `description |
module(FS=17)<<10 | summary<<21 | level<<27`. Die vier Usage-Codes tragen
`0x4400`: InvalidPath (702) `0xE0E046BE`, WriteBeyondEnd (705) `0xE0E046C1`,
UnsupportedOpenFlags (760) `0xE0C046F8`, UnexpectedFileOrDirectory (770)
`0xE0C04702`. NotFound (120) `0xC8804478`, NotFound/InvalidState
`0xC8A04478`, NotFormatted (340) `0xC8A04554`, FileAlreadyExists (180)
`0xC8A044B4`, DirectoryAlreadyExists (185) `0xC8A044B9`, DirectoryNotEmpty
(240) `0xC92044F0`, ArchiveNotMounted (101) `0xC8804465`. Der
Free-Bytes-Wert von SaveDataArchive ist in Azahar auf 32 MiB gestubbt.

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

### Framebuffer und Anzeige

* FrameBufferInfo (0x1C Byte, Azahar `core/hle/service/gsp/gsp_gpu.h`):
  `+0x00 active_fb` (0 = Puffer „…1", 1 = „…2"), `+0x04 address_left`,
  `+0x08 address_right`, `+0x0C stride` (Bytes je Spalte), `+0x10 format`
  (Bits 0..2 Pixelformat, `(format & 0x70) == 0x20` = stereoskopisch oben),
  `+0x14 shown_fb` (wird GPU-Register `active_fb`), `+0x18 unknown`.
* Ein FrameBufferUpdate-Slot (0x40 Byte, GSP-Shared-Memory
  `+0x200 + (2*thread + screen)*0x40`) enthält `index` (Byte 0, `&1` wählt
  den Eintrag), `is_dirty` (Byte 1, Bit0) und zwei FrameBufferInfo-Einträge
  ab `+0x08`/`+0x28`. GSP übernimmt einen Eintrag nur bei `is_dirty` (und
  löscht das Flag) oder wenn ein Buffer-Swap erzwungen wird.
* `gsp::Gpu SetBufferSwap` (IPC 0x0005) und der VBlank-Pfad schreiben die
  GPU-Framebuffer-Register (Azahar `GPU::SetBufferSwap`,
  `video_core/gpu.cpp`): `address_left1`/`address_left2` werden nach
  `info.active_fb` getrennt gefüllt, Register-`active_fb` ←
  `info.shown_fb & 0xFF`, dazu `stride` und `format` (untere 16 Bit). Das
  rechte Auge (`address_right1/2`) wird nur oben im 3D-Modus und bei
  `address_right != 0` getrennt geführt, sonst zeigt es auf den linken
  Puffer. `address_left == 0` wird ignoriert.
* Die Anzeige liest `address_left1` bzw. `address_left2` je nach Register
  `active_fb`. Wirksam wird der Wechsel beim VBlank (Renderer-Latch), nicht
  mitten im Frame.
* Framebuffer sind spaltenweise gespeichert: Bei `columns` Spalten
  (400 oben, 320 unten) und `rows` = 240 Zeilen liegt Pixel (x, y) bei
  `address + x*stride + (rows-1-y)*bpp`; die Zeilen laufen innerhalb der
  Spalte von unten nach oben. Azahar-Startwerte (`PicaCore::InitializeRegs`):
  width 240, height 400/320, `stride = 3*240`, RGB8.
* Pixelformate (`format & 7`): 0 RGBA8 (4 B, gespeichert A,B,G,R), 1 RGB8
  (3 B, gespeichert B,G,R), 2 RGB565, 3 RGB5A1, 4 RGBA4 (je 2 B
  little-endian). Aufhellung nach 8 Bit wie Azahar `common/color.h`:
  5→8 `v<<3|v>>2`, 6→8 `v<<2|v>>4`, 4→8 `v<<4|v`, 1→8 `v*255`.

## Y2R (YUV-Nach-RGB-Konverter, y2r:u)

Quelle: Azahar `core/hle/service/cam/y2r_u.{h,cpp}`, `core/hw/y2r.cpp`.

* Kommandos (y2r:u, Session `1`): `0x0001/2` Set/GetInputFormat,
  `0x0003/4` OutputFormat, `0x0005/6` Rotation, `0x0007/8` BlockAlignment,
  `0x0009/A` SpacialDithering, `0x000B/C` TemporalDithering,
  `0x000D/E` TransferEndInterrupt, `0x000F` GetTransferEndEvent (CopyHandle),
  `0x0010` SetSendingY, `0x0011` SetSendingU, `0x0012` SetSendingV,
  `0x0013` SetSendingYUYV, `0x0014`–`0x0017`/`0x0019` IsFinished* (Stub 1),
  `0x0018` SetReceiving, `0x001A`–`0x001D` Set/GetInputLineWidth/Lines,
  `0x001E/1F` Set/GetCoefficient (8×s16), `0x0020/21` Set/GetStandardCoefficient
  (4 Sätze), `0x0022/23` Set/GetAlpha, `0x0024/25`
  Set/GetDitheringWeightParams (16×u16), `0x0026` StartConversion,
  `0x0027` StopConversion, `0x0028` IsBusyConversion, `0x0029`
  SetPackageParameter (12 Byte: input_format, output_format, rotation,
  block_alignment, input_line_width u16, input_lines u16,
  standard_coefficient u8, padding u8, alpha u16), `0x002A` PingProcess,
  `0x002B` DriverInitialize, `0x002C` DriverFinalize, `0x002D`
  GetPackageParameter (Result + die 12 Byte).
* Sende-/Empfangspuffer (`SetSending*`/`SetReceiving`) sind je
  `{address u32, image_size u32, transfer_unit u16, gap u16}` + Prozess-Handle.
  `transfer_unit` ist die DMA-Einheit, `gap` die Lücke danach.
* InputFormat: 0 YUV422_Indiv8, 1 YUV420_Indiv8, 2 YUV422_Indiv16 (nur LSB),
  3 YUV420_Indiv16, 4 YUYV422_Interleaved (ein Strom `Y0 U Y1 V`).
  OutputFormat: 0 RGBA8, 1 RGB8, 2 RGB5A1, 3 RGB565.
  Rotation: 0/90/180/270 Grad **je 8×8-Kachel** (bei 180/270 zusätzlich
  umgekehrte Kachelreihenfolge im Streifen). BlockAlignment: 0 linear,
  1 PICA-8×8 (Morton-Order); Breite immer durch 8, bei Block8×8 auch Höhe.
* Ablauf: 8 Zeilen hohe Streifen; je Streifen Y/U/V per DMA lesen
  (U/V bei 4:2:2 halb so viele Bytes wie Y, bei 4:2:0 ein Viertel),
  YUV→RGB, Rotation, Ausgabe linear oder gekachelt, DMA-Ausgabe.
* Koeffizienten (2.8-Festkomma c0..c4, 11.5-Offsets c5..c7):
  `R=(c0*Y+c1*V)>>3+c5+0x18`, `G=(c0*Y-c2*V-c3*U)>>3+c6+0x18`,
  `B=(c0*Y+c4*U)>>3+c7+0x18`, jeweils `>>5` und auf 0..255 begrenzen
  (bit-exakt zur Hardware nach Azahar). Standards: Rec601
  `100,166,B6,58,1C5,-166F,10EE,-1C5B`, Rec709, Rec601_Scaling, Rec709_Scaling.
* `StartConversion` rechnet synchron und plant die Fertigmeldung als
  `MinY2RDelay = 50000` Zyklen (≈186 µs) danach: Event signalisieren,
  `is_busy_conversion=false`. `DriverInitialize` setzt YUV422_Indiv8/RGBA8/
  keine Rotation/Linear/Koeffizienten 0/1024×1024/alpha 0 und löscht das
  OneShot-Event; die Antwort ist `MakeBuilder(1,0)` (Result, keine Parameter).
  `GetPackageParameter` liefert `MakeBuilder(4,0)` (Result + 12 Byte).
* Ungültige Konfiguration (Format/Rotation/Alignment oder Zeilenbreite):
  Azahar bricht in `y2r.cpp` per ASSERT ab, es wird **kein**
  Completion-Interrupt/Event signalisiert und die DMA-Ausgabe bleibt aus.
  Der Port verhält sich genauso (Event bleibt unsignalisiert) und loggt
  `[y2r] ungueltige Konfiguration (...)`; das ist kein Workaround, sondern
  die dokumentierte Azahar-/Hardware-Semantik.
* Das Spiel nutzt es für Video: 400×240, YUV420_Indiv8, RGB8, Block8×8,
  Rec601_Scaling, alpha 255 (Azahar-Referenzspur). Umsetzung in
  `runtime/platform/y2r.h` (reine Funktionen), Anbindung in
  `runtime/kernel/hle.c`; Test `tests/y2r_test.py` mit synthetischen Bildern
  (alle Formate, Rotation, Morton, DMA-Lücken).

## DSP (Audio-HLE)

Quelle: Azahar `src/audio_core/hle/{hle,shared_memory,source,mixers,filter,
effects}.cpp`, `src/audio_core/{codec,interpolate}.cpp`.

* Der DSP-RAM liegt bei `0x1FF00000`; die Anwendung nutzt zwei Regionen zu je
  `0x8000` Byte: `0x1FF50000` (Region 0) und `0x1FF70000` (Region 1). Der
  Frame-Counter ist das letzte `u16` jeder Region (`+0x7FFE`) und wird von der
  Anwendung erhoeht. Gelesen wird die Region mit dem hoeheren Counter
  (Ausnahme Wraparound: `0xFFFF` gegen `0xFFFE`), geschrieben wird die andere.
* `dsp::DSP ConvertProcessAddressFromDspDram(addr)` liefert
  `(addr << 1) + 0x1FF00000 + 0x40000`; die 15 von `WriteProcessPipe(Audio,
  Initialize)` gelieferten DSP-Adressen sind `0x8000 + Offset/2` der
  `SharedMemory`-Struktur (Frame-Counter `0xBFFF`, Source-Config `0x9E92`,
  Source-Status `0x8680`, ADPCM-Koeffizienten `0xA792`, DSP-Config `0x9430`,
  DSP-Status `0x8400`, Final-Mix `0x8540`, Intermediate-Mix `0x9492`,
  Compressor `0x8710`, Debug `0x8410`).
* `SharedMemory`-Offsets: DSP-Status `0x800`, Debug `0x820`, Final-Mix
  `0xA80` (160×2 s16 LE), Source-Status `0xD00` (24×12), Compressor `0xE20`,
  DSP-Config `0x2860` (196 Byte), Intermediate-Mix `0x2924` (2×4×160 s32 LE),
  Source-Config `0x3D24` (24×192), ADPCM-Koeffizienten `0x4F24` (24×16 s16).
  `u32_dsp`-Felder (Pufferadresse, Laenge, Position …) sind mittelendian: die
  beiden 16-Bit-Haelften sind gegenueber little-endian vertauscht.
* Audio-Frame: 160 Samples bei 32728 Hz, Takt `160 * 4096 * 2 = 1310720`
  ARM11-Zyklen (4,889 ms). Pro Frame verarbeitet der DSP alle 24 Quellen
  (PCM8/PCM16/GC-ADPCM, mono/stereo, Puffer-Queue mit Prioritaet nach
  `buffer_id`, Loop, Lineare/None-Interpolation mit 24 Nachkommabits,
  Simple-/Biquad-Filter, 3 Gain-Saetze mit Ramp), mischt 3 Zwischenmixer und
  schreibt Endmix + Status in die Schreibregion. Danach wird
  `InterruptType::Pipe` fuer `DspPipe::Audio` signalisiert (Azahar
  `AudioTickCallback`).
* Pufferadressen sind FCRAM-physikalisch (`0x20000000 + x`). Die DSP-DMA
  ignoriert die unteren zwei Adressbits: Azahar dekodiert immer ab
  `physical_address & 0xFFFFFFFC`. Im Port entspricht das
  `(addr & 0xFFFFFFFC) & 0x1FFFFFFF` (Gast-Linearspeicher, `arm_mem_read`).
* Der ADPCM-Zustand `yn1`/`yn2` ist **s16** (Azahar `ADPCMState`); die
  Koeffizienten sind s16 mit 11 Nachkommabits. Ein als u16 gelesener Verlauf
  kehrt das Vorzeichen um.
* `WriteProcessPipe(Audio)`: vor jedem StateChange wird ein offener Sleep
  abgeschlossen (`FinishPendingSleep`, State Sleeping) und
  `sleep_reply_frames = 0` gesetzt. `Initialize` nullt den **gesamten**
  DSP-RAM (0x80000 Byte ab 0x1FF00000); `Initialize` und `Wakeup` leeren alle
  Pipe-Puffer (`ResetPipes`). Nach einem Sleep setzt der naechste Audio-Frame
  `sleep_reply_frames = 2`; zwei Frames spaeter wird `InterruptType::Zero`
  signalisiert.
* Lineare Interpolation: `x0 + fraction * (x1 - x0) / 2^24` mit gesaettigter
  Differenz. Azahar rechnet das in `uint64` (implizite Konversion), was fuer
  negative Differenzen einer Abrundung (floor) entspricht, nicht der
  C-Division gegen Null.
* Status je Quelle (12 Byte): `is_enabled` (0), `current_buffer_id_dirty` (1),
  `sync_count` (2), `buffer_position` (4, u32_dsp), `current_buffer_id` (8),
  `last_buffer_id` (10). Die Dirty-Bits der gelesenen Konfiguration werden
  vom DSP je Frame geloescht.
* `SetSemaphore` ist im HLE ohne Wirkung; `RecvData(0)` liefert 0 bei
  laufendem DSP, sonst 1, `RecvDataIsReady(0)` ist false solange ein Sleep
  aussteht.

## HID (hid:USER / hid:SPVR)

* Beide Dienste teilen **ein** Modul: ein Shared-Memory-Block (0x1000 Byte,
  `CreateSharedMemory(..., 0x1000)`) und fuenf OneShot-Events (PadOrTouch1,
  PadOrTouch2, Accelerometer, Gyroscope, DebugPad). `GetIPCHandles`
  (`0x000A`) liefert per `MakeBuilder(1,7)` Result + `CopyHandleDesc(6)` und
  die sechs Handle-Werte. Azahar signalisiert DebugPad nie; die uebrigen
  Events werden bei jedem Update des jeweiligen Rings signalisiert.
* Layout (Azahar `hid.h`, `ASSERT_REG_POSITION` belegt pad 0x00/touch 0x2A
  in Worten): Pad ab `0x000` (Ticks 0/8, Index 0x10, Slider 0x18, aktueller
  Zustand 0x1C, 8 Eintraege ab 0x28 je 16 Byte), Touch ab `0x0A8`
  (8 Eintraege ab 0xC8 je 8 Byte: x u16, y u16, valid u32), Accelerometer ab
  `0x108` (8 Eintraege ab 0x128 je 6 Byte), Gyroscope ab `0x158`
  (32 Eintraege ab 0x178 je 6 Byte). Die Ringindizes sind absolut; der
  Vorgaenger eines Eintrags ist `(index - 1) % anzahl` (u32-Arithmetik).
* `index_reset_ticks`/`_previous` werden nur beim Schreiben von Index 0
  aktualisiert, in Systemticks (`svcGetSystemTick`, ARM11 268.111856 MHz,
  Azahar `BASE_CLOCK_RATE_ARM11`).
* Takt (Azahar `hid.h`): Pad alle `BASE_CLOCK_RATE_ARM11 / 234` Ticks
  (~4,27 ms), Accelerometer `/ 104`, Gyroscope `/ 101`. Das Pad-Event laeuft
  ab dem Modulaufbau; Accelerometer/Gyroscope erst nach `0x0011`/`0x0013`
  (Zaehler, `0x0012`/`0x0014` dekrementieren).
* Pad-Bits: 0 A, 1 B, 2 Select, 3 Start, 4 rechts, 5 links, 6 oben, 7 unten,
  8 R, 9 L, 10 X, 11 Y; 28 Circle-Right, 29 Circle-Left, 30 Circle-Up,
  31 Circle-Down. Circle-Pad-Rohwerte -0x9A..0x9A, Azahar mittelt drei
  Abtastungen; Richtungsbits ab Radius 40 (tan30/tan60).
* Command-Antworten: `0x0015` f32 `14.375` (gyroscope_coef), `0x0016`
  `{0,+6700,-6700}` je Achse, `0x0017` `(u8)(0x3F * Lautstaerke)`.
* Shared Memory wird im Port per `svcMapMemoryBlock` (SVC 0x1F) zugeordnet;
  das Spiel mappt HID nach `0x10000000`, GSP nach `0x10002000`.

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
| `0x0A8`–`0x0AD` | Prozedurale Textur (Einheit 3): Konfiguration, Noise, LUT-Parameter |
| `0x0AF` | Proctex-LUT-Index (Bits0..7) und Tabelle (Bits8..11) |
| `0x0B0`–`0x0B7` | Proctex-LUT-Daten; jedes Wort schreibt bei `index` und erhöht ihn |

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
* **LDREX/STREX/CLREX**: Die Reservierung gilt pro (Adresse, Groesse). Sie
  wird von jedem Store auf den ueberwachten Bereich, von `CLREX`, von einem
  Exception-Eintritt (SVC) und von einem Threadwechsel entwertet (ARMv6K;
  Azahar `ThreadManager::SwitchContext` ruft `cpu->ClearExclusiveState()`).
  `STREX` schreibt nur bei gueltiger Reservierung und liefert sonst 1;
  Rueckgabe 0 bedeutet Erfolg. `LDREXD/STREXD` ueberwachen 8 Byte.
  (Port: `arm_excl_*` in `runtime/cpu/arm_cpu.c`; Unicorn-Faelle in
  `tools/recomp_difftest.py`, Invalidierungstest in `tests/mvp_selftest.py`.)

### VFP / Float (verifiziert mit Unicorn, 2026-11-24)

* **FPSCR-Startwert** des 3DS: `0x03C00010` = Default-NaN (Bit 25),
  Flush-to-Zero (Bit 24), Rundung Richtung Null (Bits 22..23 = 3),
  Inexact-Cumulative (Bit 4). Azahar setzt ihn beim Thread-Erzeugen
  (`kernel/thread.cpp`, `svc.cpp`); der Port macht das in `arm_cpu_init`.
  Die Rundung wird auf den Host uebernommen (`fesetround`).
* **Rundung**: `vcvt.<s32|u32>.<f32|f64>` rundet immer Richtung Null
  (unabhaengig von FPSCR), saettigt bei Ueberlauf (INT_MAX/INT_MIN bzw.
  UINT_MAX/0) und liefert fuer NaN 0. `vcvt.<f32|f64>.<s32|u32>`,
  `vcvt.f32.f64`, `vcvt.f64.f32` folgen FPSCR.RMode. Festkomma-Varianten
  skalieren vor/nach der Konvertierung mit `2^fbits`.
* **FTZ**: denormalisierte Eingaben werden als Null mit Vorzeichen gelesen,
  denormalisierte Ergebnisse als Null mit Vorzeichen geschrieben. **DN**:
  NaN-Ergebnisse werden zu `0x7FC00000` bzw. `0x7FF8000000000000`.
* **NaN**: eine ungueltige Operation (0/0, inf-inf, inf*0, sqrt(negativ))
  liefert den positiven Default-qNaN; VNMUL negiert danach das Vorzeichen,
  VMLS/VNMLA behandeln das negierte Produkt wie eine Subtraktion (DN
  kanonisiert am Ende der Add/Sub-Operation).
* **VCMP/VCMPE** setzen FPSCR.NZCV: gleich `Z=1,C=1`; kleiner `N=1`;
  groesser `C=1`; unordered (NaN) `C=1,V=1`. `vmrs APSR_nzcv, fpscr`
  kopiert Bits 31..28 nach CPSR; `vmrs Rt, fpscr` liest den rohen FPSCR.
* **VMLA/VMLS/VNMLA/VNMLS/VNMUL** runden das Produkt vor der Addition
  (nicht fusioniert; Unicorn/ARM11-VFPv2). `VNMLA: -d - n*m`,
  `VNMLS: -d + n*m`, `VNMUL: -(n*m)`.
* **VMOV** zwischen Kern- und Doppelregister geht nur als Wortpaar:
  `vmov dN, Rt, Rt2` / `vmov Rt, Rt2, dN` (niedriges Wort zuerst).
* VNEG/VABS sind Bitoperationen (Vorzeichen invertieren/loeschen) und
  unterliegen weder FTZ noch der NaN-Kanonisierung.

## Quellen
* https://www.3dbrew.org/wiki/IPC, /RomFS, /GSP_Shared_Memory, /GPU/Internal_Registers
* libctru: `source/services/{apt,gsp,srv,fs}.c`, `source/ndsp/ndsp.c`
* Azahar: `src/core/hle/service/**`, `src/video_core/pica/**`
