---
name: 3ds-hle
description: Debugging-Wissen fuer den statischen 3DS-Port (Professor Layton/Azran Legacy). Nutze diesen Skill bei IPC-/Reply-Layout-Fehlern, fehlenden Diensten (z. B. boss:U), GSP-/Shared-Memory-Fragen, RomFS/IVFC, HLE-Cmd-IDs und fuer die Werkzeuge trace-diff, guest-refs, guest-run, guest-watch, guest-exp. Enthaelt die verifizierten Fallen und bekannten Sackgassen des Projekts.
---

# 3ds-hle: statischer 3DS-Port debuggen

Dieser Skill ist fuer den Port in `C:\Users\user\Documents\Repos\3DS`.
Kurzpfad: **messen -> vergleichen -> Hypothese -> A/B-Test**.

## Zuerst die Werkzeuge

```powershell
python tools\g3ds.py trace-diff reports\azahar_trace.txt tools\fixtures\port_boot_20261119.log --table
python tools\g3ds.py guest-refs 0x0040B4C4
python tools\g3ds.py guest-run --timeout 60 --until "srvname.*boss:U"
python tools\g3ds.py guest-watch --write 0x40B4C4 --timeout 10
python tools\g3ds.py guest-exp record --hypothesis "..." --env HLE_X=1 --repeat 3
```

Details: `docs/TOOLS.md`. Der Selbsttest ist `python tools\selftest_tools.py`.
Die Werkzeuge schreiben nur nach `tools/out/`.

## IPC: Header und die Reply-Layout-Falle

Der ARM11-IPC-Header ist **ein Wort in `cmdbuf[0]`**:

| Bits | Feld |
|---|---|
| 0..5 | `translate_params_size` (Anzahl Woerter) |
| 6..11 | `normal_params_size` (Anzahl Woerter) |
| 16..31 | `command_id` |

Quelle: `tools/azahar/src/core/hle/ipc.h` (`union Header`, `MakeHeader`).

**Die Falle:** Die Runtime schreibt den Header nach dem Befuellen des
Reply-Puffers nach `buf[0]`:

```c
/* runtime/hle/hle.c:455 */
static void ipc_finish(Ipc *r) {
    r->buf[0] = ((r->cmd & 0xFFFFu) << 16)
                | ((uint32_t)(r->normal & 0x3F) << 6)
                | (uint32_t)(r->translate & 0x3F);
}
```

`ipc_put()` beginnt aber bei `buf[0]`. Das **erste** geschriebene Wort wird
also vom Header ueberschrieben. Wer `Result` ohne Dummy schreibt, verschiebt
alle folgenden Parameter um ein Wort nach vorne: Das SDK liest den ersten
Parameter als `Result` (bzw. `Result` als Parameter). Deshalb gilt:

> **Regel:** Fuer jede Reply mit logischem `Result + N Parametern` zuerst ein
> Dummy-Wort pushen, dann `Result`, dann die Parameter. `normal` ist in dieser
> Runtime die Anzahl der `ipc_put`-Aufrufe inklusive Dummy; der Header
> ueberschreibt davon den ersten. Beispiel ReceiveParameter: 5 Aufrufe =
> Dummy + Result + 3 Parameter, nutzbar sind `cmdbuf[1..4]`.

Im Projekt ist `put_result()` genau dieser Dummy (`hle.c:655`: `ipc_put(r, 0)`).

### ReceiveParameter (APT 0x000D/0x000E) als Beleg

```c
/* runtime/hle/hle.c:761 */
ipc_begin(r, r->cmd, 5, translate >= 2 ? 2 : 0);
ipc_put(r, 0);   /* Dummy: Header-Slot */
ipc_put(r, 0);   /* Result   -> cmdbuf[1] */
ipc_put(r, sender);  /* -> cmdbuf[2] */
ipc_put(r, signal);  /* -> cmdbuf[3] */
ipc_put(r, size);    /* -> cmdbuf[4] */
```

Antwort auf "Warum rutscht mein ReceiveParameter-Ergebnis um ein Wort?":
Weil `cmdbuf[0]` bereits der Header ist. Ohne das Dummy-Wort landet `Result`
auf `cmdbuf[0]` und wird ueberschrieben; `sender` rutscht auf `cmdbuf[1]`,
also dorthin, wo das SDK `Result` erwartet.

## Deskriptoren

| Deskriptor | Wort 0 | Wort 1 |
|---|---|---|
| CopyHandle | `0x00 \| ((n-1)<<26)` | n Handles |
| MoveHandle | `0x10 \| ((n-1)<<26)` | n Handles |
| CallingPid | `0x20` | PID |
| StaticBuffer | `0x2 \| (id<<10) \| (size<<14)` | Gastadresse |
| MappedBuffer | `0x8 \| (perms<<1) \| (size<<4)` | Buffer-ID |

Quelle: `tools/azahar/src/core/hle/ipc.h` (`DescriptorType`,
`StaticBufferDescInfo`, `MappedBufferDescInfo`). Wichtig: Beim StaticBuffer
ist die **Adresse das zweite Wort**; die Groesse steckt in Bits 14..31 des
ersten Worts. Beim MappedBuffer ist die Groesse in Bits 4..31.

Im Port werden Translate-Deskriptoren u. a. so gelesen:
`addr = p[r->normal + 1]` (`hle.c:1229`, `hle.c:1261`). Das ist die
Adresse, nicht das Flag-Wort.

## Verifizierte Dienst-Cmd-IDs

Alle IDs sind im Projekt in `runtime/hle/hle.c` implementiert und in echten
Laeufen (`[ipc]`) gesehen. Namen stammen aus Azahar/Citra.

### srv:
`1` RegisterClient, `2` EnableNotification, `3` RegisterService,
`4` UnregisterService, `5` GetServiceHandle.

### fs:USER
`0x0801` Initialize, `0x0802` OpenFile, `0x0803` OpenFileDirectly,
`0x0808` CreateFile, `0x080C` OpenArchive, `0x080D` ControlArchive,
`0x080E` CloseArchive, `0x0812` GetFreeBytes, `0x0817` IsSdmcDetected,
`0x0818` IsSdmcWriteable, `0x0861` InitializeWithSdkVersion,
`0x0862` SetPriority, `0x0863` GetPriority.

### file
`0x0802` Read, `0x0803` Write, `0x0804` GetSize, `0x0805` SetSize,
`0x0808` Close, `0x0809` Flush.

### dsp::DSP
`0x0001` RecvData, `0x0002` RecvDataIsReady,
`0x0003`/`0x0005`/`0x0009`/`0x000A` Set/Send/Clear/Mask,
`0x0004` IsEmpty, `0x0006` RecvFifoEx/GetSemaphore,
`0x0007` SetSemaphore, `0x0008` GetSemaphore, `0x0009` Clear,
`0x000A` Mask, `0x000B` CheckSemaphore, `0x000C`
ConvertProcessAddressFromDspDram, `0x000D` WriteProcessPipe, `0x000E`
ReadPipe, `0x000F` GetPipeReadableSize, `0x0010` ReadPipeIfPossible,
`0x0011` LoadComponent, `0x0012` UnloadComponent, `0x0015`
RegisterInterruptEvents, `0x0016` GetSemaphoreEventHandle,
`0x0017` SetSemaphoreMask.

### gsp::Gpu
`0x0001`-`0x0003` WriteHWRegs*, `0x0004` ReadHWRegs, `0x0005`
SetBufferSwap, `0x0006` SetCommandList, `0x0007` RequestDma,
`0x0008`-`0x000B` Cache/Events/LcdForceBlack, `0x000C`
TriggerCmdReqQueue, `0x0013` RegisterInterruptRelayQueue,
`0x0018` ImportDisplayCaptureInfo.

Im heutigen Port-Lauf sieht man nur `0x0010`, `0x0013`, `0x0016`,
`0x001E`; `0x0006`/`0x000C` werden vom Spielcode nicht erreicht.

### APT:U
`0x0001` GetLockHandle, `0x0002` Initialize, `0x0003`/`0x0004`
Enable/Finalize, `0x0005` GetAppletManInfo, `0x0009` NotifyToWait,
`0x000B`/`0x0043` InquireNotification, `0x000D` ReceiveParameter,
`0x000E` GlanceParameter, `0x0028` NotifyToWait/Enable, `0x004B`
AppletUtility, `0x004F` SetAppCpuTimeLimit, `0x0050`
GetAppCpuTimeLimit.

### cfg:u / ptm:u / ndm:u
* cfg:u: `0x0001` GetConfig, `0x0002` GetRegion,
  `0x0003` GetTransferableId, `0x0005` GetSystemModel,
  `0x000B` IsFangateSupported.
* ptm:u: `0x000B` GetStepHistory, `0x000C` GetTotalStepCount.
* ndm:u: `0x0001` EnterExclusiveState, `0x0002` LeaveExclusiveState,
  `0x0004` LockState, `0x0005` UnlockState, `0x0006` SuspendDaemons,
  `0x0007` ResumeDaemons, `0x0008` SuspendScheduler,
  `0x0009` ResumeScheduler, `0x0014` OverrideDefaultDaemons,
  `0x0015` ResetDefaultDaemons.

## RomFS / IVFC

`out/romfs_image.bin` ist das SelfNCCH-RomFS-Image. Der IVFC-Header:

| Offset | Bedeutung |
|---|---|
| `0x00` | Magic `IVFC` |
| `0x04` | Version/Magic Number (`0x00010000`) |
| `0x08` | Master-Hash-Groesse |
| `0x0C` | L1-Offset |
| `0x10` | **L1-Groesse (u32)** |
| `0x14` | L2-Offset |
| `0x18` | **L2-Groesse (u32)** |
| `0x1C` | L3-Offset |
| `0x20` | **L3-Groesse (u32)** |

Die drei Groessen stehen also als Little-Endian-`u32` bei **0x10, 0x18,
0x20**. Der Metadaten-Header beginnt bei **0x1000**:

| Offset | Feld | Wert im Layton-Image |
|---|---|---|
| `0x1000` | Header-Laenge | `0x28` |
| `0x1004` | Dir-Hash-Offset | `0x28` |
| `0x1008` | Dir-Hash-Laenge | `0x24` |
| `0x100C` | Dir-Meta-Offset | `0x4C` |
| `0x1010` | Dir-Meta-Laenge | `0x114` |
| `0x1014` | File-Hash-Offset | `0x160` |
| `0x1018` | File-Hash-Laenge | `0x994` |
| `0x101C` | File-Meta-Offset | `0xAF4` |
| `0x1020` | File-Meta-Laenge | `0xD284` |
| `0x1024` | **file_data_offset** | `0xDD80` |

`tools/romfs_image.py` extrahiert das Image aus der ROM und setzt fuer den
sequenziellen Port-Parser `L1 = 0x1000 - 0x208`, `L2 = 0`, `L3 =
file_data_offset`. Ohne diese Anpassung liest der Parser Hash-Tabellen als
Nutzdaten. Der Port oeffnet SelfNCCH ueber `OpenFileDirectly` (Cmd `0x0803`,
Archive-ID 3) und liefert `out/romfs_image.bin` (`hle.c:1034`).

## GSP: CommandBuffer und InterruptRelayQueue

* GSP-Shared-Memory wird im Port an **0x10002000** gemappt
  (`runtime/hle/hle.c:1972`).
* `CommandBuffer` liegt bei **SharedMem + 0x800** = `0x10002800`
  (Azahar: `gsp_gpu.cpp:90`, `GetCommandBuffer`).
* `InterruptRelayQueue` liegt im Projekt bei **SharedMem + 0x1000** =
  `0x10003000` (`hle.c:1975`). Struktur (0x40 Byte): `index` u8,
  `number_interrupts` u8, `error_code` u8, `config` u8, `missed_PDC0` u32,
  `missed_PDC1` u32, danach `slot[0x34]` (Interrupt-IDs).
* `slot[0] = 0x02` ist **PDC0 = VBlank**; `PDC1 = 0x03`, `PPF = 0x04`,
  `P3D = 0x05`, `DMA = 0x06` (Azahar
  `gsp_interrupt.h`, `enum class InterruptId`).
* Merke: Azahars interner Zeiger beginnt an der Shared-Memory-Basis; die
  Gastadresse im Port ist trotzdem `+0x1000`. Immer die Gastadresse
  protokollieren, nicht die Citra-Interna.
* Relevante Cmds: `0x0005` SetBufferSwap, `0x0006` SetCommandList,
  `0x000C` TriggerCmdReqQueue, `0x0013` RegisterInterruptRelayQueue.

## libctru-Referenzen

* `ndspInit` / `ndspFinalize`:
  https://github.com/devkitPro/libctru/blob/master/libctru/source/ndsp/ndsp.c
* `aptMainLoop`:
  https://github.com/devkitPro/libctru/blob/master/libctru/source/services/apt.c
* GSP:
  https://github.com/devkitPro/libctru/blob/master/libctru/source/services/gsp.c
* srv:
  https://github.com/devkitPro/libctru/blob/master/libctru/source/services/srv.c
* IPC-/RomFS-Hintergrund:
  https://www.3dbrew.org/wiki/IPC
  https://www.3dbrew.org/wiki/RomFS

## Bekannte Sackgassen (gemessen, nicht nur vermutet)

Diese Aenderungen wurden bereits gemessen und brachten **kein** `boss:U`:

1. `HLE_QUEUE_FAKE=1` (Queue-Objekt `0x408508`, count/pos setzen):
   srvnames unveraendert, kein `boss:U` (Commit `39c291b`).
2. `HLE_GSP_IRQ=1` (InterruptRelayQueue `slot[0]=PDC0`):
   kein neuer Effekt, gsp nur Init (Commit `c49b723`).
3. DSP-Reply-Layout (`0x000D`/`0x0012`/`0x0007`/`0x0017` mit zwei Woertern):
   0 Fatals, aber weiter kein `boss:U` (Commit `989ed57`).
4. `HLE_RM_ADV` / erzwungene Resource-Manager-Zaehler:
   Thread 2 verlaesst die Warteschleife, aber `boss:U` bleibt aus
   (Commit `5da49ad`).
5. `HLE_GSP_INIT`-Workaround (Client-Handle vorbelegen): GSP-Init laeuft,
   der Render-Task sendet aber weiterhin keine PICA-Liste.
6. `HLE_GPU_TEST` / `HLE_FORCE_FRAME`: belegen nur, dass der GPU-Ausfuehrer
   und der Gastaufruf funktionieren; sie erzeugen keinen Spiel-Renderzustand.
7. Die PICA-Liste ist im Normallauf leer (GSP-Shmem nur ein Nullwort), und
   `fn_0013E3E0` (Trigger `0x000C`) wird nie ausgefuehrt; `ARM_WATCH=13E3E0`
   ergab 0 Treffer (`reports/PORT.md`).

Wichtig: `boss:U` ist der **erste fehlende Dienst** im kanonischen
trace-diff, nicht automatisch die Ursache. Erst die Referenzreihenfolge
vergleichen, dann mit `guest-refs`/`guest-watch` die Hypothese pruefen.

## Messschleife

1. `guest-run --until "<Kriterium>" --label hypo1` -> Log + Metriken.
2. `trace-diff` -> erste Divergenz.
3. `guest-refs <adresse>` -> Wer schreibt/liest die Adresse?
4. `guest-watch --write <adresse>` -> Wird sie wirklich geschrieben?
5. `guest-exp record --hypothesis "..." --repeat 3` -> Ergebnis festhalten,
   Duplikate vermeiden.

Nuetzliche Env-Schalter der Runtime: `HLE_LOG=1`, `HLE_SRV_NAMES=1`,
`HLE_IPC_LOG=<n>`, `HLE_PERF=1`, `ARM_WATCH=<hex>`,
`ARM_MEMWATCH=<hex>`, `ARM_MEMWATCHRD=<hex>[,size]`,
`ARM_MEMWATCHRANGE=<hex>,<size>`, `HLE_MEMWATCHVAL=<hex>`.
