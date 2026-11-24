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
* **Texturtypen (nur Einheit 0, 3dbrew/Azahar `TextureColor`):** Typ in
  0x83 Bits28..30: 0 Texture2D, 1 TextureCube, 2 Shadow2D, 3 Projection2D,
  4 ShadowCube, 5 Disabled (liefert 0,0,0,0). Einheiten 1/2 ignorieren den
  Typ. `Projection2D` teilt u/v durch `tc0_w` (Semantik 16, TEXCOORD0_W).
  `Cube`/`ShadowCube` wählen die dominante Achse von (u,v,tc0_w), Face-Adresse
  `address*8` mit Bits0..21 aus `0x86..0x8A` (übrige Bits von 0x85),
  `u'=x/z*0.5+0.5`, `v'=y/z*0.5+0.5`. `Shadow2D` teilt bei
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
  ist der Index signed (result*128), sonst unsigned (result*256).
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

## Quellen
* https://www.3dbrew.org/wiki/IPC, /RomFS, /GSP_Shared_Memory, /GPU/Internal_Registers
* libctru: `source/services/{apt,gsp,srv,fs}.c`, `source/ndsp/ndsp.c`
* Azahar: `src/core/hle/service/**`, `src/video_core/pica/**`
