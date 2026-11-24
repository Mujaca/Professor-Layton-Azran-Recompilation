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
Bits 20..27, `group` Bit 31. Jeder Record beginnt 8-Byte-ausgerichtet; nach
einem Record mit ungerader Wortzahl folgt ein Pad-Wort. `extra > 0`:
`group=1` schreibt die Extra-Woerter nach `id+1..id+extra` (sequentielle
Register), `group=0` schreibt `id` selbst `extra`-mal (Shader-/LUT-Upload).
Die Maske wird byteweise angewandt (`Azahar ExpandBitsToBytes`, z. B. `0x5`
= Bytes 0 und 2). Quelle: `pica_core.cpp` `ProcessCmdList`/`WriteInternalReg`.

| Register | Bedeutung |
|---|---|
| `0x010` | `irq_request` (Finalize): bei Byte-Treffer gegen `irq_compare` (`0x020`) P3D-Interrupt; `irq_autostop` (`0x034`, Default 1) beendet die Liste. GSP-Defaults: `irq_compare=0x12345678`, `irq_mask=0xFFFFFFF0`, `irq_autostop=1`. |
| `0x080` | Texture-Config, Bit 0 = texture0 enable |
| `0x082` / `0x085` / `0x08E` | Tex0 Größe (h Bits 0..10, w 16..26) / Adresse>>3 / Format |
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
  0x2B0 (Bit je Uniform), `int_uniforms` 0x2B1..0x2B4,
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
  POSITION 0..3, COLOR 8..11, TEXCOORD0 12/13). Nicht gemappte Semantiken
  sind 1.0; Farben werden vor der Interpolation abs/saturiert.
* Instruktionen sind ein Wort: Opcode Bits26..31, Swizzle-Muster je
  `operand_desc_id`. Bedingungen vergleichen `refx`/`refy` mit
  `conditional_code` (Or/And/JustX/JustY). `LOOP` nutzt ein Int-Uniform
  (x = Downcounter, y = aL-Start, z = aL-Inkrement); aL ist
  `address_register[2]`. Bei nicht-invertierten Befehlen ist `src1` das
  7-Bit-Register (Uniform/adressierbar), `src2` nur 5 Bit (Input/Temp).

Das Titelbild ist ein 3D-Render (`title.xc`): korrekte Darstellung braucht
zusätzlich **TEV** und die Rasterizer-Zustände (Viewport/Clip/Depth).

## Quellen

* https://www.3dbrew.org/wiki/IPC, /RomFS, /GSP_Shared_Memory, /GPU/Internal_Registers
* libctru: `source/services/{apt,gsp,srv,fs}.c`, `source/ndsp/ndsp.c`
* Azahar: `src/core/hle/service/**`, `src/video_core/pica/**`
