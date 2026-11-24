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
  OneShot-Event.
* Das Spiel nutzt es für Video: 400×240, YUV420_Indiv8, RGB8, Block8×8,
  Rec601_Scaling, alpha 255 (Azahar-Referenzspur). Umsetzung in
  `runtime/platform/y2r.h` (reine Funktionen), Anbindung in
  `runtime/kernel/hle.c`; Test `tests/y2r_test.py` mit synthetischen Bildern
  (alle Formate, Rotation, Morton, DMA-Lücken).

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

Das Titelbild ist ein 3D-Render (`title.xc`): korrekte Darstellung braucht
einen **Vertex-Shader-Interpreter/Übersetzer** und **TEV**.
`runtime/gpu.c` hat beides noch nicht.

## Quellen

* https://www.3dbrew.org/wiki/IPC, /RomFS, /GSP_Shared_Memory, /GPU/Internal_Registers
* libctru: `source/services/{apt,gsp,srv,fs}.c`, `source/ndsp/ndsp.c`
* Azahar: `src/core/hle/service/**`, `src/video_core/pica/**`
