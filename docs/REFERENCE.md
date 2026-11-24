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
| GSP-Shared-Memory | `0x10002000` (im Port) | CommandBuffer bei `+0x800`, InterruptRelayQueue bei `+0x1000` |
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

* InterruptRelayQueue: Index u8, Anzahl u8, Fehler u8, Config u8, missed
  PDC0/PDC1 u32, dann `slot[0x34]`. IDs: PSC0=0, PSC1=1, **PDC0 (VBlank
  oben)=2**, PDC1=3, PPF=4, **P3D=5**, DMA=6 (Azahar `gsp_interrupt.h`).
* CommandBuffer je Thread: `shmem + 0x800 + thread_id * 0x200`.
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

## Quellen

* https://www.3dbrew.org/wiki/IPC, /RomFS, /GSP_Shared_Memory, /GPU/Internal_Registers
* libctru: `source/services/{apt,gsp,srv,fs}.c`, `source/ndsp/ndsp.c`
* Azahar: `src/core/hle/service/**`, `src/video_core/pica/**`
