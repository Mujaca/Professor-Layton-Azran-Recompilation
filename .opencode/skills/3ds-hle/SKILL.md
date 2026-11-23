---
name: 3ds-hle
description: Debugging-Wissen fuer den statischen 3DS-Port (Professor Layton/Azran Legacy). Nutze diesen Skill bei IPC-/Reply-Layout-Fehlern, fehlenden Diensten (z. B. boss:U), GSP-/Shared-Memory-Fragen, PICA-Registern/-Renderer (0x200..0x22F, Texturen, Alpha/Clear), RomFS/IVFC, HLE-Cmd-IDs und fuer die Werkzeuge trace-diff, guest-refs, guest-run, guest-watch, guest-exp. Enthaelt die verifizierten Fallen und bekannten Sackgassen des Projekts.
---

# 3ds-hle: statischer 3DS-Port debuggen

Dieser Skill ist fuer den Port in `C:\Users\user\Documents\Repos\3DS`.
Kurzpfad: **messen -> vergleichen -> Hypothese -> A/B-Test**.

## Aktueller Stand (2026-11-23)

* Kanonischer Lauf:
  `python tools\run_chain_direct.py --profile tools\profiles\engine_pica_draw.json --shot out\shot_tmp.bmp`
  danach `python tools\canon.py --log out\logs\chain_direct.txt --shot out\shot_tmp.bmp --engine`.
* Messung: `python tools\measure_ref.py out\shot_engine.bmp` (verifiziert):
  TOP sichtbar `69443/90400` mean `48.32` (Jitter +-2 px); BOTTOM sichtbar
  `14752/71360` mean `14.33`.
* **IMGC-Dekoder (`runtime/imgc.c`)**: ETC1 (`fmt 0x011B`, bits=4,
  32 B/Tile), lineare Tile-Ordnung als Default, 8 bpp (`0x011C`).
  `HLE_IMGC_ZORDER=1` = alte Z-Order; `HLE_IMGC_DEBUG=1` loggt Header.
  Test `python tools\test_imgc_c.py` (ETC1 14/14, 24/32 bpp, Z-Order).
* Bottom-Asset (`out/title_b_bg.bin`) ist jetzt kachelbewusst linear
  dekodiert (`tools/make_menu_assets.py`); das senkte BOTTOM von
  `25902/71360` (20.57) auf `14752/71360` (14.33).
* Der Draw-Block stammt aus dem Engine-Strom @`0x30` -> `0x0CFE6000`;
  Quads/Vertex-Attribute und Texturen emittiert aber **HLE**
  (`HLE_PICA_SPRITES`) - also noch kein reiner Engine-Render.
* Kruecken, die im kanonischen Profil **weg** sind: `HLE_PICA_QUAD`,
  `HLE_PRECALL` und der `HLE_MENU_TEX`-Override (Default sind jetzt die
  Spiel-Assets `title_top_atlas.bin`/`title_b_bg.bin`).
* **Scene-Paar-Kette laeuft jetzt** (`tools/profiles/scene_pairs.json`):
  `11A3E0 -> 27C2BC -> 2794A4 -> 2721E0` liefern Hits, wenn man
  `0x11A3E0(scene+0x59C, stage)` je Stage (1/2/3) aufruft und die
  Records ein Handle mit `[handle+0x10]`/`[+0x24] != 0` haben.
  Stage-Filter steht in **r1** (`ldrsb [rec+0x38] == r1`).
  Adresskorrektur: `0x2721E0` schreibt `ctx+0xB34/0xB38` und
  `ctx+0xB3C/0xB40` (nicht `0x8334/833C`); der Dump dort zeigt
  PICA-Paare (`FFFFF000 000F0101`), keine Float-UVs.
* `0x28BE34` ruft die Methode aus `[node+0x10]` mit `r0 = scene+0x59C`
  (nicht `[node]`); `[scene+0x59C]` = Node-Pointer.
* 0xD2-Container-Link: Loader setzt die Basis, ein Fehlerpfad
  (`0x23A778`) loescht sie wieder; HLE setzt sie im Treiber selbst.
* Record -> Block: `[node+0xC4]` = **Recordbasis**, `+0xCC` = u16 count,
  `+0xDC` = Stage-Maske (`0x107`), Blockgroesse `size = mult*rec[0x68]`.
* Die 96 Draws sind **4 Ring-Slots x 24 gedrainte Items**
  (`[slot+0xBE]`), nicht die Record-Zahl.
* Details/Belege: `docs/PORT_STATE.md` (letzte Abschnitte) und
  `docs/HANDOVER_PROMPT.md`.
* **A2 ist abgeschlossen** (Nutzer-Entscheidung). Dokumentierte
  Abweichungen: (1) kein 0 Diff - Asset-Texturen sind nicht
  pixelgleich zum Capture (0/1200 exakte 8x8-Bloecke); (2) keine
  96 Draws - 96 = 4 Ring-Slots x 24 Items; (3) echte
  Record-Positionen fehlen (Ring-Draw-Item `0x11C000/0x278B68`,
  `desc+0x140` = 0). Offener naechster Schritt: Ring-Draw-Item-
  Pipeline, dann echte Record-Rects in `HLE_CLASSA_RECTS`.

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

Im Normallauf (Spielcode allein) sieht man nur `0x0010`, `0x0013`, `0x0016`,
`0x001E`. Fuer den Menue-/Engine-Pfad gibt es zwei Stufen:

* `hle_menu_gpu_submit` (HLE-eigene Liste, `SubmitCmdList`-Ring + `0x0006` +
  `0x000C` + `0x0005`) war der erste Present-Beleg, ist aber **nicht** mehr
  der kanonische Lauf.
* Kanonisch ist der Engine-Strom @`0x30` -> `0x0CFE6000` (Details im
  PICA-Abschnitt). Der Submit laeuft ueber `HLE_FRAME_LOOP2=12FD30`; ein
  echter, natuerlicher Engine-Submit ist noch nicht erreicht.

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

## PICA-Renderer im Port (harte Fakten)

Quellen: `runtime/gpu.c` (`p_regs`, `p_load_attrs`, `p_texel`, `p_draw_arrays`),
`runtime/hle/hle.c` (Stream-Kopie + Sprite-Emitter), Azahar
`regs_pipeline.h`/`regs_texturing.h`.

Kommandostrom = Paare `(value, header)`. Header: `cmd_id` Bits 0..15,
`param_mask` Bits 16..19, `extra_data_length` Bits 20..27, `group` Bit 31.
Bei `extra>0` folgen weitere Werte; `group=1` schreibt `id+1..id+n`, sonst
`id` n-mal.

### Register-Kurzmap (verifiziert)

| Register | Bedeutung |
|---|---|
| `0x200` | Vertex-Basis: `base = (reg >> 1) * 16` (also `reg = addr >> 3` bei 16-B-Stride) |
| `0x201` | Attributformate 0..7, 4 bit je Attribut: `fmt = b & 3` (0 BYTE, 1 UBYTE, 2 SHORT, 3 FLOAT), `ncomp = ((b >> 2) & 3) + 1` |
| `0x202` | Attributformate 8..11 in Bits 0..15; `max_attribute_index` in Bits 28..31 |
| `0x203 + l*3` | Loader `l` (0..11), Wort 0: `data_offset` in Bits 0..27 |
| Loader Wort 1 | Zuordnung `comp0..comp7`, ein Nibble je Komponente |
| Loader Wort 2 | `comp8..comp11` Bits 0..15; **`byte_count`** Bits 16..23 = Stride in Bytes; `component_count` Bits 28..31 |
| `0x228` | `num_vertices` |
| `0x22A` | Vertex-Offset (Index in den Puffer) |
| `0x22E` / `0x22F` | Draw-Trigger (arrays / indexed) |
| `0x23A`/`0x238` | Command-Buffer-Adresse/Groesse (Kanal je `+0`/`+4`) |
| `0x23C`/`0x23D` | Trigger fuer verschachtelte Kommandolisten |

Ein Loader ist **3 Woerter** ab `0x203 + l*3`; es gibt keine eigene
`0xC3`-Loaderkonstante im Port. Die Stride heisst `byte_count` (Wort 2,
Bits 16..23) - genau das Feld, das in `p_load_attrs` als `bcount` gelesen
wird.

Textur-Register:

| Register | Bedeutung |
|---|---|
| `0x80` | `main_config`; **Bit 0 = texture0_enable** (die Falle: nicht `0x81`!) |
| `0x81` | `border_color` (kein Enable) |
| `0x82` | width Bits 16..26, height Bits 0..10 |
| `0x85` | Texturadresse >> 3 |
| `0x8E` | Format: `0` RGBA8, `1` RGB8, `3` RGB565, `4` RGBA4, `5` IA8, `7` I8, `8` A8 |

### Engine-Strom @`0x30`

* Der Strom ist eine **State-Liste** (`0x228`, `0x229`, `0x25E`, `0x25F`,
  `0x227`, `0x245`, `0x253`, `0x232`, `0x111`), **keine** Vertex-Loader und
  **keine** Textur-Register. Allein gezeichnet ergibt er nichts.
* `0x228` (nv) wird im Engine-Pfad von `0x27B34C` gesetzt; `HLE_PICA_BIND=1`
  ruft `0x27B34C(0x408808, 0x0CFE7100, 0x0CFE7070, 1)` direkt vor dem Submit
  und baut den Draw-Block.
* Gruppen-Records (Header-Bit 31, `extra > 0`) sind **8-Byte-aligned**: nach
  den Extras folgt ein Pad-Wort. Ohne `HLE_PICA_ALIGN8=1` liest der Parser
  danach `(value, header)` vertauscht.
* **`0x22F`-Semantik im Port:** beide Trigger (`0x22E`/`0x22F`) rufen
  denselben `p_draw_arrays`; `0x22F` liest **keinen** Indexpuffer,
  `nv` kommt aus `0x228`. Der Engine-Strom nutzt `0x22F` als reinen
  Draw-Trigger.
* Kanonisch kommen echte Attribute aus `HLE_PICA_SPRITES`: je Zeile
  `x y w h u0 v0 u1 v1` ein Quad mit eigenem Attributblock
  (`0x200..0x208`) und Draw-Block (`nv=6`). Der Strom liefert nur den
  Draw-Teil.

### Alpha, Clear und Determinismus

* `put_px_blend` mischt Quell-Alpha ueber das Ziel
  (`out = src*a + dst*(1-a)`); ein implizites Frame-Clear gibt es **nicht**.
* Ohne Clear akkumuliert der Shot ueber die Frame-Anzahl: zwei Laeufe
  ergaben TOP `65707` vs `85271`.
* Mit `HLE_PICA_CLEAR=1` (loescht das Ziel vor dem Draw, `w*h*3` Bytes) sind
  drei Laeufe byte-identisch. **Regel:** jede Bildmessung nur mit Clear und
  mindestens 2x wiederholen.

### NDC-Heuristik

Attribut 0 ist die Position. NDC -> Pixel wird nur angenommen, wenn **alle
drei** Eckpunkte in `[-1.6, 1.6]` liegen; sonst gelten Pixelkoordinaten.
Ein Quad `(0,0)-(400,240)` bleibt dadurch im Pixelraum - ohne diese Regel
landet `(0,0)` in der Bildmitte.

### HLE_PICA_* / HLE_CLASSA_* (Defaults)

| Schalter | Wirkung | Default / kanonisch |
|---|---|---|
| `HLE_PICA_LOG=<n>` | loggt die ersten `n` Draws (`[pica] draw#`) | Default `0` = still |
| `HLE_PICA_RENDER` | heuristischer Rasterizer (Attr0 = Pos, erstes 2-Komp-Attribut = UV) | Default aus; `tests/port.c` setzt es bei `HLE_ENGINE_FRAME` automatisch |
| `HLE_PICA_ALIGN8` | Pad-Wort nach Gruppen-Records | Default aus; kanonisch **an** |
| `HLE_PICA_BIND` | ruft `0x27B34C` vor dem Submit (baut Strom) | Default aus; kanonisch **an** |
| `HLE_ENGINE_STREAM=<src>,<len>` | kopiert Engine-Strom nach `0x0CFE6000` | kanonisch `30,1C8` |
| `HLE_PICA_TARGET=<addr>,<w>,<h>` | Ziel-Framebuffer + Groesse | kanonisch `1F000000,400,240` |
| `HLE_PICA_PRIMARY=<tex>,<w>,<h>,<fmt>` | bindet Top-Textur vor jedem Submit | kanonisch `1E000000,1024,1280,0` (RGBA8) |
| `HLE_PICA_SECOND=<tex>,<w>,<h>,<fb>` | zweiter Pass (Bottom) | kanonisch `1E100000,320,240,1F060000` |
| `HLE_PICA_SPRITES` / `_SPRITES2=<datei>` | je Zeile ein Quad (`x y w h u0 v0 u1 v1`) | kanonisch `title_top_rects.txt` / `bottom_bg_rect.txt` |
| `HLE_PICA_UVRECT` / `_UVRECT2=u0,v0,u1,v1` | UV-Fenster fuer den Vollbild-Fallback | Default `0,0,1,1` |
| `HLE_PICA_VERTEX` | haengt Attributblock + `nv=6` vor den Strom | Default aus; Altpfad, **nicht** kanonisch |
| `HLE_PICA_QUAD` | Vollbild aus gebundener Textur ohne Attribute | Default aus; **obsolet** im kanonischen Lauf |
| `HLE_PICA_CLEAR` | loescht das Ziel vor dem Draw | Default aus; kanonisch **an** (Determinismus) |
| `HLE_MENU_TEX` / `_BOT` | Pfad-Override fuer den Atlas | Default `out/title_top_atlas.bin` / `out/title_b_bg.bin` (Spiel-Assets, nicht Referenz) |
| `HLE_MENU_TEX_W`/`_H`/`_BPP` | Atlasgroesse und Bytes/texel (`3` oder `4`) | kanonisch `1024`/`1280`/`4`; das PICA-Format kommt aus `HLE_PICA_PRIMARY` |
| `HLE_CLASSA_DRIVE` / `_RING` / `_RECTS=<datei>` | 0xD2-Chunk-Treiber / Ring-Dump / Rect-Export | Diagnose, nicht kanonisch |
| `HLE_PRESENT_FILL=0` | schaltet den `#604020`-Present-Fill ab | Default **an** |

## Widerlegte fruehere Annahmen (nicht mehr als Praemisse nutzen)

1. **"Der Strom @`0x30` ist der echte PICA-Draw/Quad."** Falsch: es ist eine
   State-Liste ohne Vertex-Loader und ohne Textur-Register. Beleg:
   `@0030 id=228 val=1`, `0x200/0x201/0x202 = 0`, kein `[pica] quad`.
2. **"96 Draw-Items."** Falsch: `0x11DB84` wird 96x gerufen = **4 Ring-Slots
   x 24 gedrainte Items** (`[slot+0xBE]`, Drain-Schleife `0x1ED98C`). Der
   Record-Pfad liefert `count` Draws (Probe: 3).
3. **"Titel-Texturen sind L4-Nibble; NCC 0.125."** Falsch: `title.xc`
   enthaelt **ETC1** (IMGC `fmt=0x011B`, bd=4, 32 B/Tile). Der 0.125-NCC war
   ein Decode-Artefakt des falschen L4-Dekoders.
4. **"`HLE_PICA_QUAD`/`HLE_PRECALL` sind noetig."** Nur fuer den alten
   Vollbild-Pfad; kanonisch sind echte `0x200..0x208`-Attribute +
   `HLE_PICA_SPRITES` aktiv.
5. **"0 Pixel Diff."** Gilt nur fuer die Referenz-Capture-Kopie
   (`reports/menu_official*.bin`), nicht fuer Spiel-Assets. Aktuell:
   TOP `69370/90400` (47.98), BOTTOM `25902/71360` (20.57).

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
   ergab 0 Treffer (`reports/PORT.md`). **Stand 2026-11-22.** Inzwischen
   laeuft der Engine-Strom @`0x30` ueber `HLE_ENGINE_STREAM`/`HLE_PICA_BIND`
   (siehe „Aktueller Stand"); der natuerliche Submit ist weiter offen.

## Skill-Pflege (Prozessregel)

Nach **jedem** belegten Fakt nicht nur `docs/PORT_STATE.md` ergaenzen,
sondern im selben Commit eine Zeile hier in `3ds-hle` bzw. in
`.opencode/skills/3ds-port/SKILL.md` aktualisieren. Sonst altern die Skills
schneller als der Port - genau das ist bei `0x30`, „96 Draws" und L4 passiert.

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
