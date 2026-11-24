# WORKAROUNDS – technische Schuld

Alles, was vom **unveränderten Spielcode auf korrekter 3DS-Semantik** abweicht.
Ziel ist eine leere Liste (Meilenstein M8). Neue Einträge nur mit Begründung
und vermuteter Ursache. Jeder entfernte Eintrag ist echter Fortschritt.

Anzeigen/abbauen:

```powershell
python tools/build.py --pristine                       # ohne Code-Patches bauen
python tools/workarounds/apply.py --skip 0x0010F8C4    # einzelnen Patch auslassen
rg "WORKAROUND\(" runtime                              # Runtime-Workarounds
```

## 1. Patches am Spielcode – `tools/workarounds/code_patches.json` (0)

**Leer.** Alle 22 Einträge sind entfernt (2026-11-24). Beleg je Gruppe:

| Gruppe | Entfernt (Adressen) | Frontier danach |
|---|---|---|
| Locale/`mblen` (widecopy, narrowcopy, conv_success, mblen) | 0x13023C, 0x130270, 0x13028C, 0x130308, 0x13A0D4, 0x13A0DC, 0x13A168, 0x13A170, Caves 0x399F00/0x399FA0 | 9/10 (boss:U) |
| Archiv-Loader / Größen-Stub | 0x1300C8, 0x1301C0, 0x13C46C, 0x13C4EC, 0x28F33C, Cave 0x399F40 | 9/10 (boss:U) |
| Menü-Gate, Frame-Callback, Thumb-Interworking | 0x10F8C4, 0x11BA98, 0x2927B8 | 9/10 (boss:U) |
| RomFS-Lookup (namelen/chain/empty component) | 0x212F3C, 0x399244, Cave 0x399FE8 | **9/10 (boss:U)** |

Belege: `HLE_PERFMAP_FILE`-Lauf zeigte für 0x1302xx/0x13A0xx keinen
ausgeführten Block; die Locale-Konvertierung `fn_00130220 -> 0x13A080`
wird im natürlichen Boot nicht betreten. Die übrigen Gruppen wurden
einzeln gebaut und mit `tools/frontier.py` gemessen: der Lookup läuft
mit den unveränderten Originalbytes (also ohne Namenslängen-/Kettenende-
Hack und ohne Größen-Stub) bis zu demselben Punkt. Das Spiel braucht die
Patches offenbar nur für spätere Assets, nicht für den Boot bis `dsp::DSP`.

**Früher entfernt:** `0x3CBEFC` (`jp`→`uk`, `lang_uk`). Der Patch war
überflüssig: Das Spiel setzt beim Boot Region 2 (EUR) und Sprache 2 in
`0x3D8E68/0x3D8E69`; seine interne Sprachcodetabelle ab `0x3D8EEC` liefert
für Index 2 bereits `uk`. Beleg: `guest-watch --write 0x003D8E68`, Frontier
9/10 mit 22 statt 23 Patches. Die `cfg:u`-Blöcke liefern jetzt echte
EUR-Werte (Sprache EN, Land GB, Modell Old3DS plausibel: der Zeiger auf
`0x3CBEFE` („jp“) steht in der Tabelle `0x3D8EEC` nur an Index 0
(`0x12B60C` wählt `table[sprache]`); der Patch änderte also ausschließlich
den JP-Eintrag, nie den von EUR verwendeten Index 2.

`tools/fix_romfs_cycles.py` und `tools/rebuild_romfs_hash.py` waren
Workarounds am Image und sind entfernt (2026-11-24); der Port liest jetzt
`out/romfs_image.bin`, wie es aus der ROM extrahiert wird. `tools/romfs_image.py`
setzt zwar noch die IVFC-Größen, die Engine liest diese Bytes aber nicht:
Alle Lesezugriffe beginnen per `+0x1000` beim Level-3-Header (siehe §6,
RomFS-Mount), sodass die IVFC-Bytes nur toter Ballast im Image sind.

## 2. Block-Hooks im Recompiler – `tools/workarounds/block_hooks.json` (0)

**Leer.** Entfernt wurden `0x1079D0`/`0x107A30` (IPC-Reply-Größe),
`0x11A818` (Busy-Wait), `0x399244` (Stack-Slot) und der Hook nach
`bl 0x287DD4` (Allocator-Ersatz). Beleg: Build ohne alle Hooks, Frontier
bleibt 9/10 (boss:U). Die Kernel-Synchronisation (`runtime/kernel/hle.c`)
und die GSP-Interrupts liefern inzwischen die echten Werte/Zustände, die
die Hooks vorher ersetzt haben.

Früher entfernt: `0x12E1E4`/`0x12E1F0` (`arm_mutex_bootstrap`) – `ARM_WATCH`
zeigte im gesamten Boot 0 Treffer (toter Code).

## 3. Textersetzungen im generierten C – `tools/workarounds/gen_patches.py` (0)

**Leer** (2026-11-24). Die letzte Ersetzung – `fn_0028EE00` verglich die
srv-Namenslänge mit `cmp r2,#8`, ersetzt durch `#9` – ist entfernt.

**Beleg, dass sie unnötig war:** `[srvname]`-Log (Diagnose, `HLE_LOG`) im
natürlichen Boot mit unverändertem `cmp r2,#8`:

```
[srvname] "hid:USER" len=8 name8=524553553A646968 ... -> 17
[srvname] "gsp::Gpu" len=8 name8=7570473A3A707367 ... -> 31
[srvname] "fs:USER"  len=7 ...
```

`name_len` ist also exakt `strlen(name)` (nicht `strlen+1`), wie Azahar
`sm/srv.cpp` (`kMaxPortSize = 8`, Ablehnung nur bei `name_len > 8`). Frontier
mit `cmp #8`: **9/10 (boss:U)**, identisch zu vorher.

**Ursache der alten Abweichung:** Der rekompilierte strlen `fn_0028F5E8`
(wortweise Schleife mit `uqsub8` + `lsls`-Kaskade) liest per
`ldr r1,[r0],#4`; der post-index-Writeback fehlte in einer früheren
`emit.py`-Version – im alten `build/generated_all.c` (17.11.) steht nur
`c->r[1] = arm_ld(c, a, 4, 0)` ohne `c->r[0] = a + 4`, die Schleife las
also immer dasselbe Wort. Inzwischen erzeugt
`tools/recomp/emit.py` den Writeback (`c->r[0] = a + 4`). Test:
`tests/mvp_selftest.py::strlen_wortschleife` (Originalsequenz als asm,
8-Zeichen-String im Gastpuffer, erwartet r0 = 8). Der Testharness hatte den
Textbereich (Literal-Pool `0x01010101` bei `0x10005C`) nicht im Gastpuffer –
deshalb lief der frühere Versuch in eine Endlosschleife; Puffer ist jetzt
`0x140000` Byte groß, `mem_bytes` legt Bytefolgen ab.

## 4. Runtime – `rg "WORKAROUND(" runtime`

| Kennung | Ort | Wirkung |
|---|---|---|
| – | `dispatch_ipc` | leitet Kommandos nach Form statt nach Handle um (APT/cfg/fs) |
| – | Thread-Scheduler | alle Gast-Threads laufen kooperativ auf **einem** Host-Thread; Prioritaeten gelten nur an SVC-/Yield-Grenzen, `GetCurrentProcessorNumber`/`GetThreadIdealProcessor` liefern die ExHeader-/CreateThread-Id (0 AppCore, 1 SysCore), ein zweiter Kern wird nicht simuliert |
| – | generierter Code | unbekannte Aufrufziele = No-op (geloggt in `out/missing_calls.txt`) |
| – | `arm_mem_read/write` | Zugriffe außerhalb des Speichers liefern 0 statt Abbruch (`ARM_STRICT=1` bricht ab) |
| – | fs `OpenFile`, `file_command` | Sentinel-Dateislot und Erfolg auch bei fehlgeschlagenem Open; `romfs_image.bin` mit Offset +0x1000 |
| – | `gpu_pica_virt_to_phys` | Adressen außerhalb VRAM/Linear-Heap bleiben unverändert (der Port hat keine MMU/Seitentabelle); echtes GSP lehnt sie ab |
| – | `p_process_tri` | Rasterizer nach Azahar: Sutherland-Hodgman-Clipping (7 Ebenen + 0x47/0x48..0x4B), Viewport 0x41/0x43/0x68, Perspective-Divide, 12.4-Fixed-Point-Coverage + Top-Left-Bias, perspektivkorrekte Attribute, TEV 0x0C0..0x0FD inkl. Combiner-Buffer, Texturen 0x080..0x09F (alle 14 Formate inkl. ETC1/ETC1A4 mit Subtile `x/4+2*(y/4)`, 8x8-Morton, Wrap/Border, keine Filterung wie Azahar), Fog 0xE0/0xE1/0xE6/0xE8..0xEF (LUT-Upload mit Offset-Inkrement, Flip, Index/Fraktion, vor dem Depth-Test), Output-Merger 0x100..0x107 (Alpha-Blend, LogicOp, Alpha-Test, Kanalmasken) und Farb-/Tiefenziel 0x117/0x11C..0x11E (RGBA8/RGB8/RGB565/RGB5A1/RGBA4 in Azahar-Byte-Reihenfolge, D16/D24/D24S8, Depth-/Stencil-Test, 8x8-Morton, Schreibgrenzen des Farbziels). Fragment-Lighting 0x140..0x1D9 nach `sw_lighting.cpp` (Lichtquellen, Distanz-/Spot-Daempfung, Fresnel, Bump-Mapping, Clamp, LUT-Upload 0x1C5/0x1C8..0x1CF) in die TEV-Quellen Primary/SecondaryFragment sowie Texturtypen auf Einheit 0 (Projection2D, Cube/ShadowCube, Shadow2D inkl. Bias, tc0_w). **Noch offen:** Mipmaps, Proctex (Einheit 3). |

Ohne Vertex-Programm (0x2CB/0x2CC) zeichnet der Port nichts mehr; der frühere
Attribut-Fallback (Attribut 0 als Position, 1 als UV) und der Schreibpfad in
den linearen RGB8-Host-Puffer `g_fb` ohne konfiguriertes Farbziel (0x11D) sind
entfernt (2026-11-24). Damit gibt es keinen Pfad mehr, der ein Bild ohne
GSP/PICA-Kommandos erzeugt.


Entfernt 2026-11-24 (Fälschungen nach Azahar). Jeder Punkt wurde einzeln
gebaut und mit `tools/frontier.py` gemessen; der Stand blieb **9/10
(`boss:U`)**, die DSP-Pipe liefert jetzt aber die echten Struktur-Adressen
(30 `ConvertProcessAddress`-Aufrufe wie in der Referenz).

| Früher | Jetzt | Beleg |
|---|---|---|
| Y2R `GetTransferEndEvent` vorab signalisiert, `IsFinished*` immer wahr | ein `completion_event` (OneShot) ohne Vorab-Signal; `StartConversion` plant die Fertigmeldung (MinY2RDelay ≈ 186 µs), `IsFinished*` spiegeln `is_busy_conversion` | `cam/y2r_u.cpp`; im Boot nur bis `GetTransferEndEvent` erreicht |
| DSP `GetSemaphoreEventHandle` vorab signalisiert | OneShot-Event ohne Vorab-Signal | `dsp/dsp_dsp.cpp` |
| DSP `ReadPipeIfPossible` schreibt 1/0 | echte Pipe-Puffer; `WriteProcessPipe(Audio, Init)` liefert die 15 DSP-Struktur-Adressen, `ReadPipe`/`ReadPipeIfPossible`/`GetPipeReadableSize` lesen daraus | `audio_core/hle/hle.cpp` PipeRead/PipeWrite/`AudioPipeWriteStructAddresses` |
| srv `GetServiceHandle` unbekannt = Erfolg | `name_len > 8` → `0xD9006405`, unbekannter Name → `0xD0406401`; alle im Boot gefragten Namen sind bekannt | `sm/srv.cpp`, `sm/sm.cpp` |
| `generic_command` still = Erfolg | loggt jedes unbekannte Kommando mit Parametern; Antwort weiter Result 0 (Azahar `ReportUnimplementedFunction`) | `service.cpp` |
| cfg unbekannte Blöcke = Nullen | `0xD9010004` (NotFound/Config/WrongArgument) | `cfg/cfg.cpp` GetConfigBlockPointer |
| `SleepThread` 50-ms-Deckel | volle Schlafzeit, `ns <= 0` = Yield | `kernel/svc.cpp` |
| `DuplicateHandle` kopierte nur den Namen | Alias-Slot/Refcount auf dasselbe Objekt; Pseudo-Handles `0xFFFF8000/1` sind echte Slots | `kernel/handle_table.cpp` |
| APT `GetLockHandle`/`Initialize` neue Objekte | liefern dasselbe Lock/Notification-/Parameter-Event | `apt/applet_manager.cpp` |
| HID `GetIPCHandles` legte bei jedem Aufruf neue Handles an; Shared Memory blieb leer, Events wurden nie signalisiert | ein Modul je Prozess (0x1000 Shared Memory + 5 OneShot-Events); Pad-/Touch-/Accel-/Gyro-Ringe mit Azahar-Takt und Systemticks, `svcMapMemoryBlock` blendet den Block ein | `hid/hid.cpp`, `hid/hid.h`; Beleg `[map] MapMemoryBlock h=2 addr=10000000`, `[hid] pads=N ticks=...` |

**Host-Eingabe (kein Workaround):** `runtime/platform/window.c` bildet Tastatur
(A/B/X/Y, L/R, Start/Select, Steuerkreuz, Circle-Pad) und die Maus ueber dem
unteren Screen auf `HleHidInput` ab (`runtime/platform/hid_ring.h`); die
Tastenbelegung steht in `port.ini` neben `port.exe` (Standard und Format:
`docs/FRONTEND.md`), ein XInput-Pad wird optional zusaetzlich gelesen. Das
Circle-Pad der Tastatur wird wie Azahar `analog_from_button`/`hid.cpp` auf den
Einheitskreis normiert und mit `0x9A` skaliert (Diagonale also 109, nicht
154/154). `hle_hid_set_input` schreibt nur in den Host-Puffer; die HID-Ringe
werden ausschliesslich in `hid_poll` unter dem Gast-Lock aktualisiert (aus
`hle_pump`, den Warteschleifen und dem VBlank-Thread). Kopiert werden nur die
von Azahar geschriebenen Ringbereiche, nicht die ganze 0x1000-Byte-Seite.
Ohne Eingabe schreibt HID den neutralen Zustand mit fortlaufenden
Zeitstempeln, Touch ohne Druck `x=y=0`, Accelerometer-Ruhelage `(0,-512,0)`
(Erdbeschleunigung des Motion-Emulators).

**Host-Bildausgabe (kein Workaround):** `runtime/platform/framebuffer.h` liest
die vom Spiel gemeldeten Puffer spaltenweise (`address + x*stride +
(239-y)*bpp`) und decodiert die fuenf GSP-Pixelformate; Fenster und
`--shot datei.bmp` zeigen beide Screens untereinander (oben 400x240, unten
320x240). Das Fenster skaliert ganzzahlig und seitenverhaeltnistreu
(Letterbox), F11 schaltet Vollbild, die Titelzeile zeigt Host-fps und
Gast-Frames (`docs/FRONTEND.md`); Anzeige und GSP-VBlank laufen im exakten
3DS-Takt 268111856/4481136 Hz (`runtime/platform/frame_clock.h`).
`runtime/platform/gsp_fb.h` bildet `GPU::SetBufferSwap` nach
(address_left1/2 nach `active_fb`, Register-`active_fb` aus `shown_fb`), und
`runtime/kernel/hle.c` uebernimmt den sichtbaren Puffer erst beim VBlank. Der
VBlank-Thread liest die FrameBufferInfo und loescht ihr dirty-Flag atomar
unter dem Gast-Lock (wie die GSP-Signalisierung in die InterruptRelayQueue),
damit ein Buffer-Swap des Spiels nicht verloren gehen kann.
Getestet mit synthetischen Puffern in `tests/framebuffer_test.py`. Im
natuerlichen Boot schreibt das Spiel inzwischen seine FrameBufferInfo in den
GSP-Shared-Memory; der VBlank-Pfad uebernimmt sie
(`[gsp] SetBufferSwap screen=0 fb=0x1F346500 stride=0x321 fmt=1 (VBlank)`,
reproduzierbar). Damit ist M2 belegt (eigener Framebuffer des Spiels, kein
Host-Eingriff); der IPC-Aufruf `0x0005` selbst kommt weiterhin nicht vor.


**Y2R (kein Workaround):** `runtime/platform/y2r.h` bildet die echte
YUV→RGB-Umwandlung nach (Azahar `core/hw/y2r.cpp`): YUV422/YUV420 in 8/16 Bit
und YUYV, Ausgabe RGBA8/RGB8/RGB5A1/RGB565, Rotation je 8×8-Kachel, lineare
oder PICA-8×8-Ausgabe, Koeffizientensätze inkl. `SetCoefficient`,
`transfer_unit`/`gap` als DMA-Einheiten, Alpha. Alle Konfigurationskommandos
werden gespeichert; `StartConversion` rechnet in die von
`SetSendingY/U/V/YUYV` und `SetReceiving` gemeldeten Gast-Puffer und
signalisiert danach das `GetTransferEndEvent`-Event nach `MinY2RDelay`
(≈186 µs). Getestet mit synthetischen Bildern in `tests/y2r_test.py`; das
Spiel nutzt den Konverter erst bei der Video-Wiedergabe, im natürlichen Boot
werden nur `DriverInitialize`, `SetTransferEndInterrupt` und
`GetTransferEndEvent` erreicht.

Offen (dokumentierte Abweichung): srv-`GetServiceHandle` mit `flags`-Bit0 == 0
müsste in Azahar auf eine spätere Registrierung warten; der Port hat keinen
Dienst-Registrierungsdienst und meldet sofort den Fehler. Ein zweiter
`APT:Initialize`-Aufruf liefert in Azahar `AlreadyExists`, hier erneut Erfolg
mit denselben Objekten. Beide Pfade kommen im Boot bisher nicht vor.

**fs-Archive (2026-11-24):** `runtime/kernel/fs_archive.c` bildet SaveData
(0x4), ExtSaveData (0x6), SharedExtSaveData (0x7) und SystemSaveData (0x8) auf
`out/savedata/{savedata,extsavedata,sharedextsavedata,systemsavedata}/...` ab.
Kommandos, Result-Codes und das 0x228-Byte-Directory-Entry-Layout folgen Azahar
(`fs_user.cpp`, `archive_*.cpp`, `disk_archive.cpp`, `path_parser.cpp`);
Directory-Objekte sind echte Kernel-Handles mit eigenem Stream. Bewusste
Host-Abweichungen:

* SaveData nutzt `program_id = 0`, weil der Port `fs:REG`/das Codeset-
  Program-ID nicht abbildet (Azahar-`ClientSlot::program_id` ist ohne
  Registrierung ebenfalls 0). Azahar liefert fuer ein fehlendes SaveData
  `NotFormatted` (0xC8A04554); der Port ebenso, das Spiel formatiert selbst.
* SharedExtSaveData ist auf echter Hardware ein bereits vorhandenes
  Systemarchiv. Der Archiv-Layer legt es **nicht** mehr an: ein fehlendes
  Shared-ExtSaveData liefert wie Azahar `NotFormatted` (0xC8A04554), ein
  fehlendes normales ExtSaveData `NotFound/InvalidState` (0xC8A04478).
  `hle_init` stellt das Titel-Archiv `00048000/F000000B` (`/gamecoin.dat`,
  belegt aus `reports/azahar_trace.txt` OpenArchive 7 + OpenFile mode=3) beim
  Start in `out/savedata` bereit (`fs_archive_provision_shared`); das ist
  Host-Zustand, kein Gastzustand und kein Spielcode-Eingriff.

Weitere Angleichungen an Azahar (2026-11-24, Review): die vier
Usage-Result-Codes tragen das FS-Modul 17 (0x4400): InvalidPath 0xE0E046BE,
UnsupportedOpenFlags 0xE0C046F8, UnexpectedFileOrDirectory 0xE0C04702,
WriteBeyondEnd 0xE0E046C1. `OpenFile` lehnt `mode == 0` und Create ohne Write
ab; ExtSaveData lehnt das Create-Flag und `CreateFile(size 0)` ab, setzt
Dateien auf feste Groesse (`FixSizeDiskFile`: Schreiben hinter das Ende ->
WriteBeyondEnd, am Ende 0 Bytes, sonst kappen). `RenameDirectory` hat eine
eigene Funktion. `GetFreeBytes` liefert 32 MiB (Azahar-Stub). `0x0831`
(CreateSharedExtSaveData) bleibt wie in Azahar ohne Handler; `0x0851` liest
die Parameter in Azahar-Reihenfolge (dirs, files, size u64). Alle
Gastspeicherzugriffe der Pfaddekodierung und der Directory-Reads
(`count * 0x228`) sind gegen `mem_size` geprueft.


Diese Liste stammt aus dem Review der Nachtarbeit (2026-11-24) gegen Azahar.

`wa-kernel-signal-all` ist **entfernt**: `vblank_thread` flutet keine Handles
mehr und postet keine Arbiter-Wörter; auch `WaitSynchronizationN` signalisiert
keine Handles mehr pauschal. Echte Quellen: GSP meldet PDC0/PDC1 je
VBlank (Host-Takt 16 ms) über die InterruptRelayQueue an das registrierte
Event; P3D/PPF/PSC0/PSC1/DMA werden von den GX-Kommandos ausgelöst
(`gsp_process_cmdqueue`). Beleg: Frontier 4 → 9 Dienste (Divergenz boss:U)
mit identischen 23 Patches.

Seit dem PICA-Parser-Paket meldet der Kommandostrom P3D selbst: Register
`0x010` (Finalize) vergleicht `irq_request` mit `irq_compare` und signalisiert
über `gpu_pica_set_irq_handler` das GSP-Relay; `0x23C/0x23D` lösen
Kanal-Sprünge aus. `gsp_process_cmdqueue` sendet P3D nur noch bei ungültiger
Liste (Azahar `GPU::SubmitCmdList`), nicht mehr pauschal nach jeder Liste.

Seit dem Kernel-Sync-Paket sind auch `wa-wait-handle0` und
`wa-thread-exit-unsignaled` entfernt. `hle_svc` fuehrt Mutex, Semaphore,
Event (OneShot/Sticky/Pulse), Timer und AddressArbiter mit echten
ShouldWait/Acquire-Zustaenden; `WaitSynchronization1/N` warten kooperativ,
unterstuetzen `wait_all`, Timeout und liefern den Index in `r1` (der
SDK-Stub 0x287B90 speichert ihn nach `*out`). Ungueltige Handles liefern
`ResultInvalidHandle` statt „signalisiert“. Thread-Ende signalisiert den
Thread-Handle dauerhaft und gibt gehaltene Mutexe frei; `CloseHandle` gibt
Handle und Dateislot frei.


## 5. Erledigt: IPC-Reply-Layouts

Der frühere Verdacht (Antworten begannen mit `ipc_put(0)` als Result statt mit
dem Header-Platzhalter, dadurch alle Wörter um eins verschoben) ist behoben:
`ipc_begin()` reserviert seither das Header-Wort, und jede Antwort wurde
gegen Azahar `IPC::RequestBuilder` geprüft (srv, APT, fs, file, hid, cfg,
ndm, ptm, gsp, dsp, y2r, boss). Damit entfielen `wa-fs-client-slot` sowie die
`g_srv_layout`-Varianten; das Spiel liest den fs-Handle jetzt selbst aus der
Antwort.

## 6. Befunde (offene Ursachen, kein Workaround eingebaut)

### Zeitbasis des Movie-Players: kein belegter Zeitfehler (Messung 2026-11-24)

**Hypothese:** Der Film-Player rechnet Bildzeit/Lesevorlauf aus
`svcGetSystemTick`/Timern/VBlank/Audio-Position; eine falsche Zeitbasis oder
ein nicht fortschreitender DSP-Status steuert den Lesevorlauf falsch.

**Belege (Diagnose, kein Gastzustand):**
* Die Zeitbasis liegt tatsächlich auf dem Decoder-Pfad: `fn_001ED8FC`
  (laeuft ~1x/Frame) ruft `fn_00288AAC`, das ueber `fn_002740A0` zweimal
  `svc #0x28` liest und ein 64-Bit-Tickbudget gegen die globale Frame-Zeit
  (`[ptr+0x178/0x17c]`) abrechnet (Frame-Limiter); anschliessend speichert
  `fn_001ED8FC` die Tickdifferenz nach `[obj+0x78/0x80]` und ruft
  `0x00390A20`. Die uebrigen 16 `svc #0x28`-Stellen liegen in
  Bibliotheksfunktionen (0x125A80, 0x144504, 0x147FD0, 0x148D50, 0x275DF8,
  0x27C7A0, 0x27CBDC, 0x27E3C8, 0x33FA08), keine im Player-Code 0x218xxx/
  0x311xxx.
* DSP: der Audio-Thread liefert exakt 204,4 Frames/s (160 Samples bei
  32728 Hz; `[audio] f=N` mit Host-Zeit im 5,00-s-Raster), die Regionen-
  handshake-Felder werden also im richtigen Takt geschrieben. Waehrend der
  gesamten Laeufe ist **keine** DSP-Quelle aktiv (`enabled=0`, auch im
  120-s-Lauf); der Film wurde auf diesem HEAD allerdings nicht erreicht
  (siehe Reproduzierbarkeit), eine Tonspur lief daher nie.
* Endliche Wartezeiten: `HLE_WAITTMO_LOG=1` meldet in 120 s Vollspeed
  **keinen** abgelaufenen Wait (kein vorzeitiger Tick-Timeout).
* Gast-/Host-Takt: 57-58 Gast-Frames/s gegen 60 Hz VBlank (Fenster-/VBlank-
  Takt 268111856/4481136 = 59,83 Hz), d. h. die Zeitbasis laeuft nicht
  auseinander; `svcGetSystemTick` (268,111856 MHz, QPC) ist monoton.
* **Reproduzierbarkeit:** Auf HEAD 59dbf5b erreicht der natuerliche Boot in
  60 s **und** 120 s Vollspeed (ohne `HLE_LOG`) **keine** Y2R-Konvertierung
  (`HLE_Y2R_LOG=1`: 0 Zeilen, Block am Ende 0x0016E4A8), die in
  bb5fe4c dokumentierten 41 `StartConversion` sind hier also nicht
  reproduzierbar. Der Haenger ist der Koeffizienten-/Huffman-Loop
  `fn_0016E4A0`, der aus einem Bitpuffer ohne Nutzdaten (Nullen) endlos
  Symbole erzeugt; der Bit-Refill `0x16C568` (`ldrh [r1],#2`) hat keine
  Laengenpruefung.

**Ergebnis:** Die Zeitbasis selbst ist als Ursache **nicht belegt**
(DSP-Takt und Tick-Rate stimmen, keine Timeouts, keine aktive Audioquelle).
Offen bleibt die Datenzufuhr: der Decoder beginnt mit dem Huffman-Lauf,
bevor der Producer-Thread (1-ms-Poll) die Bytes geliefert hat. Der
Gast-Lock wird in `hle_pump()` nur alle 16384 Bloecke abgegeben, ein langer
Decoder-Lauf verzoegert den Producer also stark; nach dem Stall laufen
weitere Daten ein, der Loop prueft sie aber nie wieder.

### Bilddecoder hing in der Huffman-Schleife: Ruecksprung in die Funktionsmitte – behoben 2026-11-24

**Befund:** Der natuerliche Boot blieb nach dem Dsp-Setup (~2,5 s Referenzzeit)
in `fn_0016E4A0` stehen. `HLE_PERFMAP` zeigte fuer Gast-Thread 0 nur noch die
beiden PCs `0x0016E4A8`/`0x0016E4B4` (19 M Bloecke in 10 s, 182 M in 60 s,
sonst nichts). Das ist die Huffman-/Codebook-Schleife des Bilddecoders
(`ldrh [tab+idx]; tst #1; beq`), die bei einer Tabelle aus Nullen nie endet.

**Ursache (Recompiler, nicht Spiel):** Die IDCT-Helfer werden per
`add lr, pc, #4; add pc, rN, rM` angesprungen (Tabelle `0x16A510` →
`0x16EF20`… `0x16FB04`) und kehren mit `mov pc, lr` **mitten in die aufrufende
Funktion** `fn_0016A590` zurueck (LR `0x0016A600`). `Emitter.dynamic_pc` hat
ein Fremdziel als `arm_call_guest(c, t); return;` behandelt: der
C-Aufruf kehrte zum Direktaufrufer zurueck, der Epilog `0x16A680`
(`pop {r0-r6, fp, ip, pc}`) lief nie. Damit blieben r0/r1 als fortgeschriebener
Koeffizientenzeiger stehen (0x1010/0x1012, danach 0x2010/0x2014), und der
naechste Decoder-Schritt las seine Huffman-Tabelle aus `[0x2010+0x3B8] = 0`
→ Endlosschleife.

**Belege:**

* `ARM_WATCH=16A798`: r0 = 0x0CA92AE8 (gueltiges Decoder-Objekt), r3 = 0.
* `ARM_WATCH=16AFB4`: 2. Aufruf (lr 0x16A7A8) hat r0 = 0x00001010.
* `ARM_WATCH=16E4A0`: r0 = 0x00002010, lr = 0x0016C520, r2 = 0xFFFFFF9E.
* `ARM_WATCH=16A680`: **0 Treffer** – der Epilog lief nie.
* `ARM_MEMWATCH=1667FC44`: `push {r0-r3}` schreibt zuerst 0x0CA92AE8, spaeter
  0x00001010 (2. Aufruf) – der Wert wird also gar nicht zurueckgeladen.
* `[sp]`-Korrekturen bei `blk=0x0016EF38 lr=0x0016A600` zeigten den
  Tail-Ruecksprung zusaetzlich.

**Korrektur (`tools/recomp/emit.py`, `dynamic_pc`):** Nach
`arm_call_guest(c, t)` fuer ein Fremdziel wird `c->r[15]` gegen die
Blockanfaenge der *aktuellen* Funktion geprueft und per `goto` fortgesetzt
(sonst `return`). Das ist echte ARM-Semantik: `mov pc, lr` springt an die
Adresse im LR, auch wenn sie in der Funktionsmitte liegt. Nur Blockanfaenge
sind anspringbar; jede `bl`-Rueckkehradresse ist ein Block. Test:
`tools/selftest_tools.py::test_dynamic_pc_resume`.

**Folge:** Der Decoder laeuft durch, der Boot erreicht den Y2R-Introvideo-Zyklus
wie die Referenz (im 40-s-Frontier-Lauf 41x `StartConversion` samt
`SetInputFormat`/`SetSending*`/`SetReceiving`, dazu 0x4000-Byte-Videohaeds und
DSP-Pipe-Writes). Frontier bleibt 10/10 (M1/M2 ja); M3 haengt jetzt nur
noch an der Spieldauer (Titelbildschirm nach dem Film), nicht mehr an einem
Haenger. Der aeltere Abschnitt „Stall nach der zweiten PICA-Liste“ ist damit
ueberholt.

**Offen (nächster Blocker, belegt 2026-11-24):** Ab dem 42. Videoframe fallen
Tabellenziele im Decoder an, die als Block in einer bereits dekodierten Funktion
liegen, aber kein Funktionsanfang sind: `0x16E70C` (72x), `0x16AF84` (42x),
`0x16E8BC` (33x), `0x16E9E0` (27x), `0x16AA20` (27x), `0x16AF9C` (24x),
`0x16EB68` (15x), `0x16AA08` (15x), `0x16C254` (6x), `0x16C27C` (3x)
(`out/missing_calls.txt`, 150-s-Lauf). `dynamic_pc` deckt nur Bloecke der
*aktuellen* Funktion ab, `arm_call_guest` nur Funktionsanfaenge. Entweder
diese Ziele im Scanner als Funktionsanfang behalten (Funktion splitten) oder im
Emitter ein globales Block-Dispatch (Blockadresse → Label) einfuehren. Danach
den 150-s-Lauf wiederholen (aktuell bleibt der Film nach 41 Y2R-Frames stehen).

### Y2R-Videopfad fehlt: Movie-Player bleibt in Zustand 5 – offen 2026-11-24

**Befund:** Die IPC-Folge des natuerlichen Boots stimmt mit
`reports/azahar_trace.txt` bis einschliesslich `File::OpenLinkFile`
(Referenz 2.469 s, Zeile 473) im Inhalt und in den Parametern ueberein;
wegen der mehreren Gast-Threads ist nur die Multimenge der Kommandos
vergleichbar, nicht die globale Zeilenreihenfolge. Ab dem
naechsten Referenzereignis divergiert sie: die Referenz beginnt bei 2.628 s
mit dem Y2R-Framepfad (je Frame `SetInputFormat`, `SetInputLineWidth`,
`SetInputLines`, `SetSendingY/U/V`, `SetOutputFormat`, `SetRotation`,
`SetBlockAlignment`, `SetTransferEndInterrupt`, `SetAlpha`,
`SetStandardCoefficient`, `SetReceiving`, `StartConversion`, danach 4-5x
`IsBusyConversion`; 520 Zyklen bis zum Trace-Ende 11 s). Der Port sendet
den vollstaendigen Satz inzwischen selbst: im 40-s-Lauf **41** Zyklen
(`grep -c "cmd=0x0026" out/logs/frontier.txt` = 41, dazu je Zyklus
`0x0001/0x0003/0x0005/0x0007/0x000D/0x0010/0x0011/0x0012/0x0018/0x001A/
0x001C/0x0020/0x0022`). Die Differenz zu den 520 Referenzzyklen ist der
getrennt dokumentierte 41-Frame-Stall des Decoders, kein IPC-Fehler.

**Belege (Stand vor dem Decoder-Fix, nur Diagnose, kein Gastzustand):**
* `ARM_WATCH=0x00347C68` (StartConversion-Wrapper des Spiels) und
  `ARM_WATCH=0x00218660`/`0x00171DAC` (Y2R-Treiber der Frame-Schleife):
  0 Treffer im gesamten Lauf.
* Beide Y2R-Framepfade werden nur aus dem Movie-Player-Objekt aufgerufen:
  `fn_0031282C` verlangt Zustand 4 oder 8 (`fn_002779F8` liest Byte `+8`).
  `ARM_MEMWATCH` auf dieses Byte zeigt 0 -> 0 -> 1 (Start,
  `fn_00311CE0`) -> **5** (`fn_00311F58`, 0x31232C) und danach in 40 s
  **keine** weitere Aenderung.
* Zustand 5 wird von `fn_00218248` (Presenter) behandelt; der Presenter
  verlaesst die Funktion aber sofort wieder, weil der Stream-Peek
  (`fn_00218AE4` -> `0x003905E8`, Stream-ID `[obj+0x1e8]`) leer ist –
  das erste dekodierte Videoframe steht nicht an.
* Die Filmdatei selbst liegt korrekt vor: der Port liest
  `m999.moflex` (RomFS-Datei, 0x912A4 Byte, Level-3-Datenoffset
  0x26B10510) vollstaendig und alle Reads liefern die volle Laenge; die
  BCSTM-Tonspur (`MOV_999.dspadpcm.bcstm`) wird ebenfalls gelesen.
* `HLE_PERFMAP_FILE`: Thread 0 arbeitet durchgehend im Huffman-/Koeffizienten-
  Decoder (`fn_0016E4A0+0x14`, 23,7 M Bloecke bei 300 Frames, 51,3 M bei
  600 Frames), d. h. der Videodecoder laeuft – es fehlt die
  Praesentations-/Y2R-Stufe.
* Der Decoder-API-Aufruf `0x0033C864` -> `0x003909DC` wird **nie**
  ausgefuehrt (ARM_WATCH: 0 Treffer in 8 s). Dagegen laeuft der Block
  `0x001ED8FC` (in `fn_001ED4B8`, ruft `0x00390A20` =
  Puffer-/Singleton-Freigabe der Decoder-Schicht) ca. 56x/s, also einmal
  je Host-Frame. Die Praesentations-Queue (`[stream+0x34]` Ring,
  Leseindex `+0x38`, Schreibindex `+0x3c`) bleibt leer.
* Der Frame-Loop selbst (GSP `0x000C TriggerCmdReqQueue`, GX
  `0x05 FlushCacheRegions` je Liste, `SetBufferSwap` per VBlank) und alle
  Einmal-Ereignisse (`fs` OpenArchive/GetFormatInfo/OpenFile /head.lt6,
  `boss` InitializeSession/GetOptoutFlag, `ptm` GetTotalStepCount,
  `cfg` GetConfig-Blocks, `ndm` SuspendDaemons) stimmen; die Referenz
  enthaelt nach 2.6 s ausschliesslich Y2R-Zeilen.

**Naechster Schritt:** ~~Im Demuxer ansetzen: pruefen, warum das erste
Videopaket des `m999.moflex` nicht in den Praesentations-Stream gelangt.~~
**Erledigt:** Der Praesentationspfad laeuft (Decoder-Fix am Recompiler,
`dynamic_pc`); offen sind nur noch der 41-Frame-Stall und die
Y2R-Sendeparameter (naechster Abschnitt). Kein Kernel-/IPC-Fehler belegt;
ein Workaround wurde nicht eingebaut.

### Stall nach der zweiten PICA-Liste: Futex-Mutex bei `0x0040F120` – offen 2026-11-24

**Befund:** Der natuerliche Boot bleibt nach der zweiten PICA-Liste
(`size=250 ops=103 draws=0`) stehen: `lists=2`, kein `SetBufferSwap`
(M2 flaky). Vor dem Stall stimmt die IPC-/SVC-Folge mit der Referenz
(Azahar-Zeilen 300-465) ueberein; `trace-diff --kind ipc` meldet als erstes
nur Azahars interne Zeile `cfg.LoadConfigNANDSaveFile` (kein Kommando).

**Belege:**
* `ARM_MEMWATCH=40F124` (Owner-Feld des rekursiven C-Mutex): letzter
  Schreibzugriff setzt den Owner auf TLS `0x1FF8C000` (Gast-Thread `0xC`);
  danach wird er nie geloescht.
* `HLE_KLOG=1`: drei Waiter auf `svcArbitrateAddress(handle, 0x0040F120,
  type=1, value=0)` – tid 0, 0xB, 0xC, jeweils `mem=-2`; danach erscheint
  **kein weiterer** `[karb]`-Eintrag (Log-Limit 300 nicht erreicht), d. h.
  keiner der drei wurde geweckt bzw. hat den Mutex erneut angefordert.
* `HLE_WAIT_LOG`/`[kblock]`: tid 0xC steht in keinem SVC-Wait; die Perfmap
  (39 s) zeigt 19,1 M Bloecke fuer Thread 12 (heiss `fn_00269E94`,
  `fn_00287A28`/`0x08`). Der Halter laeuft also weiter und verlaesst den
  kritischen Abschnitt nicht – kein verlorener Syscall-Wakeup.
* Beteiligte Funktionen: `fn_0028F04C`/`fn_0028F014` sind
  `__gthread_mutex`-Lock/Unlock um `0x0040F120` (Rekursionszaehler
  `0x287A08`/`0x287A28`, bei 0 der Futex-Wake ueber `svc #0x22` in
  `0x2658F8`).

**Naechster Schritt:** Den kritischen Abschnitt von tid 0xC lokalisieren
(PC-Ring/Perfmap direkt nach dem letzten Owner-Schreibzugriff) und das
Objekt bestimmen, auf das der Halter dort wartet. Keine Spieladresse im
Kernel noetig.

**Nebenbefund:** Der Notification-Thread (tid 1) sendet `srv 0x000B
ReceiveNotification`, das in der Referenz nicht vorkommt (Azahar: nullptr-
Handler, liefert ebenfalls Erfolg). `EnableNotification` legt das
Sema inzwischen mit `MAX_PENDING_NOTIFICATIONS = 16` an (Azahar
`sm/srv.cpp`; vorher 2). `srv 0x000B` kommt im aktuellen 40-s-Lauf nicht
mehr vor.

### IPC-Folge bis OpenLinkFile – Dienst-Routing/Reply behoben 2026-11-24

**Ziel:** erste inhaltliche Abweichung der IPC-/SVC-Folge nach dem Oeffnen
aller 10 Dienste gegen `reports/azahar_trace.txt` finden und an der Ursache
beheben.

**Befund 1 (Dienst-Routing, HEAD vor diesem Paket):** Das Spiel oeffnet
`fs:USER` per `srv GetServiceHandle` (Referenzzeile 340) und schickt danach
`0x0861 InitializeWithSdkVersion`, `0x0862 SetPriority`, spaeter
`0x0845 GetFormatInfo` auf diesem Session-Handle. `dispatch_ipc` route alle
`0x08xx` pauschal nach Kommandoform; `0x0845` (normal=3) landete dadurch im
**Datei**-Handler `file_command` und bekam `generic_command` mit
`MakeBuilder(1,0)` (nur Result) statt Azahars `MakeBuilder(5,0)`
(`FS_USER::GetFormatInfo`, `fs_user.cpp:1190`). Beleg im HEAD-Lauf:
`[ipc] ... file cmd=0x0845 n=3 t=2 h=15` direkt nach
`fs OpenArchive id=4` und vor `OpenFile /head.lt6`; im anschliessenden
40-s-Lauf fehlt `file 0x080C` (OpenLinkFile) vollstaendig.

**Befund 2 (Antwort/Handle):** Auf dem SelfNCCH-Dateihandle folgt nach
`file Read/Close` von `/head.lt6` Azahars `File::OpenLinkFile`
(Referenz 2.469 s, `file.cpp:388`: `MakeBuilder(1,2)` + MoveHandle).
Mit der Form-Route kam `0x080C` stattdessen als `fs:USER OpenArchive` an
und lieferte einen falschen Archiv-Handle.

**Korrektur (`runtime/kernel/hle.c`):**
* `dispatch_ipc`: bei bekanntem Datei-/FS-Handle entscheidet das Handle;
  der Form-Fallback bleibt nur fuer unbekannte/fremde Handles (z. B. die
  `0x0802/0x0803/0x0808`-Aufrufe auf dem "sync"-Handle).
* `file_command 0x080C OpenLinkFile`: neue `FileSlot`-Sitzung auf denselben
  Pfad (eigene Position/Groesse, gemeinsames Backend), Reply
  `MakeBuilder(1,2)` + MoveHandle.
* `fs_command 0x0845 GetFormatInfo`: Reply `MakeBuilder(5,0)`; Werte aus dem
  tatsaechlich bereitgestellten Save-Archiv `out/savedata`
  (total_size, Anzahl Verzeichnisse/Dateien, duplicate=0).

**Erreichte Uebereinstimmung:** Die von Azahar geloggte Dienstfolge ist
vollstaendig (10/10); die Reihenfolge und Parameter der geloggten
Service-Methoden stimmen bis einschliesslich `File::OpenLinkFile` und
`File::SetPriority` (Referenz 2.469 s, Zeilen 340-473). Beleg ist der
codegleiche 120-s-Lauf des verworfenen Stands (`scratch/night/rejected-09.diff`,
identische HLE-Hunks); im aktuellen Arbeitsbaum liegt dieses Fenster hinter
dem unten genannten Host-Timing-Stall. Azahars interne `file_sys`-Meldungen
(`LoadHeader`, `LoadSectionExeFS`, `ReadRomFS`, ...) sind keine IPC-Kommandos
und werden nicht verglichen.

**Naechste offene Divergenz:** Auf `File::SetPriority` folgt in der Referenz
der Y2R-Zyklus (`SetInputFormat` ... `StartConversion`, 2.628 s). Der Port
liest zu diesem Zeitpunkt das verlinkte File (`file 0x0802` 0x10000 Byte)
und erreicht den Y2R-Zyklus nicht. Ausserdem bleibt ein **Host-Timing-Stall**
offen: unter paralleler Host-Last bleibt der Boot intermittierend nach der
zweiten PICA-Liste stehen (`lists=2`, kein `SetBufferSwap`, Threads warten auf
LightLock `0x0040F120`/Event 32); derselbe Stall trat auch vor diesem Paket
auf. Deshalb bleibt der Frontier-Gate in `docs/ROADMAP.md` bei 9/10, obwohl
einzelne gesunde 40-s-Laeufe 10/10 und M2 liefern.

**Offener Recompiler-Befund (nicht in diesem Paket geaendert):**
`out/missing_calls.txt` enthaelt weiterhin 3622 Aufrufe des Ziels
`0x0029226D` (ARM-Veneer `0x00292264: add r12, pc, #1; bx r12` -> Thumb
`0x0029226C`). `scan.py` legt derzeit nur `ldr pc`/`mov pc`-Ziele an, nicht
`adr rN; bx rN`. Der verworfene Stand enthaelt dafuer eine Korrektur samt
Test (`scratch/night/rejected-09.diff`, `adr+bx-Veneer-Ziel im Scan`); sie
braucht eine komplette Neugenerierung (`build.py`, danach `check.py --full`)
und wurde hier aus Zeitgruenden nicht uebernommen. Der Boot erreicht 10/10
auch ohne sie, die printf-Formathelfer-Aufrufe sind aber No-ops.

### VFP-Semantik korrigiert – kein Workaround 2026-11-24

Der Recompiler erzeugte VFP-Code mit vier Abweichungen: `vcmp` setzte das
C-Flag bei Gleichheit/Groesser nicht, `vcvt.f32.s32`/`f32<->f64` nutzten die
Host-Rundung (nearest) statt FPSCR (3DS-Default: Richtung Null), `vmla` u.a.
ignorierten FPSCR (Default-NaN/FTZ) und Doppelpraezisions-VMLA sowie
`vmov dN, Ra, Rb` fehlten. Korrigiert in `runtime/cpu/arm_cpu.c` (Helfer) und
`tools/recomp/emit.py`; Belege/Fuzz: `tools/vfp_difftest.py` (Unicorn-Referenz)
und feste Faelle in `tests/mvp_selftest.py`. Kein Code-Patch, kein Eintrag in
`tools/workarounds/`.

### Offset-Tabellen-Spruenge im Bilddecoder – behoben 2026-11-24

**Befund:** Der natuerliche Boot blieb nach 46-48 Gast-Frames (rund 317
PICA-Listen) stehen. Im Frontier-Log folgte auf die letzte Liste
`[svc] Break bei Block 0x00276344` (svcBreak aus dem libc-Fehlerpfad) und
danach nur noch Spins. `out/missing_calls.txt` sammelte 151.544 Aufrufe des
Ziels `0x0016A6F8` von Block `0x0016A7D0`; `HLE_STALLDUMP` zeigte Thread 0
dauerhaft zwischen `0x0016BC64` und `0x0016A7D0`.

**Ursache (Recompiler):** `fn_0016A7D0` ist der Format-Dispatcher eines
Bilddecoders. Er springt per `ldr r5,[r6,r5,lsl #2]` + `add pc,r5,r6` ueber
eine Tabelle aus **relativen 32-Bit-Offsets** (Basis `0x16A6C4`). Die
Handler (`0x16A6F8`, `0x16A720`, ...) liegen *vor* dem Funktionsanfang und
wurden vom Scanner nie dekodiert; der dynamische Sprung endete im No-op
(`arm_call_guest` ohne Ziel). Der Decoder kehrte dadurch ohne Ergebnis
zurueck und liess SP unbalanciert (`[sp] ... (korrigiert)`). Ein
Folgepfad rief `ClearEvent(0)`/`WaitSynchronizationN(0)` auf; das
fehlschlagende `ClearEvent` ging in `0x28ECC0` -> `0x29225C` (`_exit`), der
Prozess endete.

**Korrektur (`tools/recomp/scan.py`):** `offset_table_targets()` liest die
Tabelle, wenn vor einem `add pc,rX,rB` ein `ldr rX,[rB,rI]` bzw.
`ldr rX,[rB,rI,lsl #2]` mit pc-relativer Basis steht (Basis und Offset
duerfen in beliebiger Reihenfolge stehen), und seedet jeden dekodierbaren
Eintrag als eigene Funktion. Die relativen Offsets werden mit 32-Bit-Wrap
gerechnet, damit auch negative Eintraege (Tabelle `0x16CF38` ->
`0x16CEE4`) vor die Tabelle zeigen. Test:
`tools/selftest_tools.py::test_offset_table_seed`.

**Beleg:** `0x16A6F8`/`0x16A720`/`0x16A738`/`0x16CEE4` stehen danach als
Funktionen im generierten C; die Missing-Calls fuer diese Ziele sind 0
(vorher 151.544 kumuliert fuer `0x16A6F8`), es bleibt nur der Datenzeiger
`0x0035649C` (2x). Der Boot laeuft ohne Break weiter: 30-s-Lauf 3023
PICA-Listen (vorher 317 in 45 s), 300-s-Lauf 31.348 Listen und 4401
Gast-Frames, `[sp]`-Korrekturen 4 statt dutzender. Frontier bleibt 10/10,
M1/M2 ja.
**M3 weiter offen:** alle Listen haben weiterhin `draws=0`; der
Intro-Film wird ueber Y2R in den Framebuffer geschrieben (kein PICA-Draw),
die Listen sind Setup fuer die spaetere Szene.

### Gast-Threads liefen nie – behoben 2026-11-24

**Befund:** `hle_init()` lief auf dem Host-Hauptthread (`runtime/main.c`) und
nahm dort den Gast-Lock. Der Gastcode läuft aber im `game_thread`; dessen
`hle_pump()`/`guest_yield()` sahen `t_lock_owned == 0` und gaben den Lock nie
ab. Jeder per `svcCreateThread` gestartete Gast-Thread blockierte dauerhaft
in `guest_lock_enter()`. Belege (Commit 60a3682): `HLE_PERFMAP` zeigte Blöcke
nur für Thread 2, Threads 3–6 exakt 0; `[thrlock]` erschien nie.

**Korrektur (dieses Paket):** `hle_guest_enter()`/`hle_guest_leave()` in
`runtime/kernel/hle.[ch]`; `hle_init()` nimmt den Lock nicht mehr, `game_thread`
nimmt ihn vor `fn_00100000` und gibt ihn danach ab. Beleg: `[thrlock]` für
id=1/2, `HLE_PERFMAP_FILE` mit Blöcken für Thread 2 (161.263), 3 (10.796)
und 4 (29.278.181).

### Neue erste Divergenz: `ptm:u` (Frontier 7 → 4)

Mit laufenden Worker-Threads endet der Hauptthread zu früh; `services` fällt
von 7 auf 4 (APT:U, ndm:u, fs:USER, hid:USER), erste Divergenz `ptm:u`.

**Belegkette:**
* `fn_00100CE0` ruft bei `0x100D08` `fn_001028A0`; darin ruft `fn_0028E410`
  den Helfer `fn_00119D34`, der `g_os+9 = 1` setzt (`g_os = 0x003D8D58`,
  belegt mit `ARM_MEMWATCHRANGE=3D8D60,3D8D64`).
* `fn_002921A8` liest `g_os+6` (bleibt 0) – der Auslöser ist **nicht** `+6`,
  sondern `fn_00292198`, das `g_os+9` als signed byte liest.
  `+9 == 1` ⇒ `beq 0x100D98` wird nicht genommen, es geht nach `0x100D24`.
* Dort wird das Init-Flag `0x003D8CE0` gelöscht und die Shutdown-Routine
  (`fn_002920D4`, …) ausgeführt. Danach liefert `fn_001016B8` über
  `fn_001036B4` 0 (`beq 0x1014E4`), `fn_00100F98` bricht die Init ab.
* `svc 0x03` (`_exit`) bei `0x29225C`, LR `0x00100B94`; danach `halted=1`.
  Ein übrig gebliebener Worker-Thread läuft weiter und wiederholt
  `srv` cmd `0x000B`, erreicht aber kein neues Spiel-Init.

**Vermutete Ursache:** kein Fehler der Lock-Korrektur, sondern
OS-Init-/Semantik-Reihenfolge: Erst weil die Worker-Threads jetzt laufen,
vollendet `fn_001028A0` den OS-Init-Pfad und setzt `g_os+9`. `fn_00100CE0`
interpretiert das beim selben Aufruf als „schon initialisiert“ und nimmt den
Finalize-Zweig. Auf echter Hardware bleibt `g_os+9` an dieser Stelle offenbar
0 (oder die Initialisierung erfolgt später/außerhalb dieses Pfads). Zu klären
ist die Semantik von `fn_00292198` (`g_os+9`) und warum `fn_001028A0` diesen
Pfad im Port durchläuft (möglicher Recompiler-/HLE-Unterschied bei
OS-Init/Syscall-Rückgaben, kein Spielcode-Patch).


### Funktionserkennung: Scheinaufrufe aus falschem Dekodermodus – behoben 2026-11-24

**Befund:** Im Frontier-Log standen 52x `unbekanntes call-Ziel 0x0010056D
(von Block 0x00100280)` plus 4x `0x0010001C`; ein echter Aufruf wurde als
No-op verschluckt. Ursache war die Funktionserkennung, nicht der Spielcode:

* Eine Thumb-Funktion `blx #imm` springt nach ARM. `scan.decode_function`
  setzte das Ziel aber immer auf den Modus des Aufrufers. Dadurch wurde
  `0x00292258` (ARM: `mov r0,r0; mov r0,#0; b 0x26c5c4`) im Thumb-Modus
  dekodiert; der Scheinblock `0x0029259E` (`movs r0,r0; blx r0` auf
  Padding) erzeugte den Scheinaufruf.
* `pointer_seeds` ignorierte das Thumb-Bit des Zeigerworts und uebersprang
  die ersten 4 KiB (`addr < 0x101000`). `0x0010056C` ist eine echte
  Thumb-Funktion; ihr Zeiger `0x0010056D` steht im Literalpool `0x1005B8`
  und wird von ihr in ein Objektfeld (+8) gespeichert. Der Aufrufer
  `0x00100280` (`ldr r0,[r0,#8]; blx r0`) lief deshalb ins Leere.

**Korrektur:** `call_target_thumb()` (Interworking-Regel), Thumb-Bit und
voller Codebereich in `pointer_seeds()`, Verwerfen von schwachen Seeds
mitten in dekodiertem Code (ausser dort steht ein Prolog). Test:
`tests/mvp_selftest.py::discovery_tests`. Belege/Auswertung:
`scratch/recomp/analyze.py` (Funktionen 17613 -> 12969, Scheinfunktionen
weg; 90.64% der Textbytes abgedeckt), Frontier **9/10 -> 10/10**.

**Offen:** `0x0029226D` (ARM-Veneer `add ip,pc,#1; bx ip` auf die
Thumb-Funktion `0x0029226C`) bleibt ein No-op; der Scanner loest
`add rN, pc, #imm` + `bx rN` noch nicht als Sprungziel auf. Alle anderen
Fehlziele des vorherigen Laufs (`0x0010056D`, `0x00123D54`) sind weg.

### Stack-Drift + frueher Finalize-Zweig: `bx rN`-Rueckkehr – behoben 2026-11-24

**Befund:** `fn_0028F648` (Thumb) kehrt ueber `pop {r3}; add sp,#0x10;
bx r3` zurueck. Der Recompiler uebersetzte `bx r3` als indirekten
Gastaufruf (`arm_call_guest`). r3 ist die Rueckkehradresse (Thumb-Bit,
z. B. `0x1016E1`); der Dispatcher suchte `addr & ~1` und fand in
`tools/recomp/seeds.txt` den Eintrag `0x1016E0` – das ist aber nur die
Rueckkehradresse von `blx 0x28F648` in `fn_001016B8`, keine Funktion (kein
Prolog, keine Referenz im Image). Der Dispatcher startete dort
`fn_001016E0` ohne Prolog; der Epilog (`add sp,#0x48; pop {r4,pc}`) hob SP
um `+0x50` an und lud ein fremdes LR.

**Belege:** `ARM_WATCH=1016E0` → Block bei `lr=0x0028F667`; danach
`[sp] blk=0x00101744 lr=0x001036C4: SP 0FFEF9E0 -> 0FFEFA30`. Genau an der
`hid:USER cmd=0x000A`-Stelle blieb der Hauptthread danach stehen.

**Korrektur:** `return_bx_in_block()` (tools/recomp/cli.py) erkennt `bx rN`,
wenn rN zuletzt durch `pop`/`ldm sp!` aus dem Stack geladen wurde; emit.py
erzeugt dafuer `return;` statt `arm_call_guest`. Falscher Seed `001016E0`
entfernt. Test: `tests/mvp_selftest.py::bx_rN_rueckkehr_thumb`.

**Offen (Stand Kernel-Sync-Paket, durch den letzten Abschnitt ueberholt):**
Der Fehlerpfad `fn_001028A0 -> fn_0028E410 -> fn_00119D34` (`g_os+9 = 1`)
wird weiterhin ausgeloest; `err:f` meldet `E0A046DA/DB`. Der letzte Abschnitt
zeigt: nicht der RomFS-/Hash-Parser, sondern die nie initialisierte
FS-Session loest den Abbruch aus.

Der Hauptthread blieb nach `dsp::DSP cmd 0x0007 (SetSemaphore)` stehen: er
wartete auf ein eigenes Event (Handle 35, `svcCreateEvent` direkt nach
`dsp cmd 0x0011 LoadComponent`), das er zuvor per `dsp cmd 0x0015
RegisterInterruptEvents` an den DSP gebunden hat. Der Port hat keinen
Audio-/DSP-Kern, der Pipe-Interrupts liefert; Azahar signalisiert diese im
HLE-Audio (`DspHle`) beim Pipe-Processing. **Inzwischen behoben** durch die
Interrupt-Registrierung und die echten Pipe-Puffer in `runtime/kernel/hle.c`
(siehe unten).

### FS-Session fehlte wegen bedingtem `bx` ohne Fallthrough – behoben 2026-11-24

**Befund:** Der RomFS-Mount scheiterte an der nie initialisierten FS-Session
(`fn_0028EDCC` liest `[0x3D8D8C] == 0`, `err:f E0A046DB`). Die Ursache war
**nicht** ein fehlender Aufruf von `fsInit`, sondern ein Recompiler-Fehler:

* `fsInit` beginnt bei `0x291C64` (Guard `[0x3D8D8C] != 0 -> bxne lr`, danach
  `srvGetServiceHandle("fs:USER")`). `ARM_WATCH=291C64` zeigte: der Block
  wurde betreten (lr=`0x100DE8`), aber `0x28F730`/`0x28EE00` liefen nie.
* Im generierten C bestand `fn_00291C64` nur aus dem Guard und endete mit
  `return;`. `bxne lr` (0x291C70) ist aber **bedingt**: bei Z=0 muss die
  Ausfuehrung bei `0x291C74` weiterlaufen.
* `tools/recomp/scan.py::decode_function` legte den Fallthrough-Block nur
  fuer `b<cond>`/`CBZ`/`CBNZ` an. `bx<cond>`, `pop<cond> {pc}` usw. beendeten
  die Funktion; `0x291C74` existierte nur als falscher Prolog-Seed.

**Korrektur:** Jeder bedingt ausgefuehrte Terminator (`ins.cc != ARM_CC_AL`)
legt den Folgeblock an. Test: `tests/mvp_selftest.py::bx_cond_fallthrough`.
Folge: `srvname "fs:USER" -> 13`, RomFS-Mount ohne `err:f`,
Frontier 3/10 -> 9/10 (Divergenz `boss:U`).

### BLX-Zielmodus im Recompiler – behoben 2026-11-24

**Befund:** Der Scan seedete jedes `blx`-Ziel als Thumb
(`tgt_thumb = ins.id == BLX or fn.thumb`). Thumb-BLX springt aber nach ARM.
Damit wurden echte ARM-Funktionen im Bereich 0x2926xx/0x2927xx als Thumb
dekodiert: `0x2927B8` (Linker-Veneer, `ldr r0,[pc]; bx lr`, liefert `__errno`)
lief als Thumb-Müll und riss die C++-Personality-Routine in den
Terminate-Pfad (`0x1009E4 -> 0x10026C -> 0x10056D`).

**Belege:** Ringpuffer (Thread 2) endete mit `0x2926DC -> blx 0x2927B8` und
danach `0x2922FC/0x292370` (Terminate); das Image zeigt 0x292678 =
`70 40 2d e9` = ARM `push {r4,r5,r6,lr}`, der generierte Code hatte dort aber
`arm_trace_block(..., 1u)` (Thumb). Alle Aufrufer setzten `c->t = 0u` (ARM).

**Korrektur:** `call_target_thumb()` in `tools/recomp/scan.py` (BL behält den
Modus, BLX wechselt) + Test `tools/selftest_tools.py::call_mode`.
Folge: kein Abbruch mehr nach dem ersten GSP-Commandlist-Flush; das Spiel
setzt erstmals selbst `SetBufferSwap` (M2 belegt). Der Boot bleibt danach in
einem Kernel-Wait stehen; `boss:U` ist weiter offen.

### DSP: RegisterInterruptEvents und Semaphore (2026-11-24)



`dsp cmd 0x0015 RegisterInterruptEvents` merkt sich das Ereignis je
(InterruptType, Pipe). `SetSemaphore` ist im HLE ein No-op wie in Azahar
`DspHle::SetSemaphore`; seit `runtime/audio/dsp_hle.c` existiert, signalisiert
der Audio-Thread `InterruptType::Pipe`/`DspPipe::Audio` nach jedem Audio-Frame
(160 Samples, 4,889 ms) und verarbeitet die beiden DSP-Regionen. Vorher blieb
der Hauptthread direkt nach `SetSemaphore` auf seinem eigenen Event (Handle
35, `HLE_KLOG`) stehen; jetzt laufen `ReadPipeIfPossible`/
`ConvertProcessAddress` weiter. **Pipe-Inhalt (2026-11-24):** `WriteProcessPipe(Audio, Init)` liefert
jetzt die 15 DSP-Struktur-Adressen, die das Spiel zu 30
`ConvertProcessAddress`-Aufrufen verarbeitet (wie `reports/azahar_trace.txt`);
`ReadPipe`/`ReadPipeIfPossible` lesen echte Puffer. `boss:U` bleibt offen.


### Arbiter korrekt, Boot stoppt am Gast-RomFS-Lookup (2026-11-24)

`svcArbitrateAddress` folgt jetzt Azahar (1/2 ohne Timeout, 3/4 mit Timeout
und immer `ResultTimeout`, Decrement nur bei 2/4). Fehlende Weckquelle war
APT: das erste registrierte Applet bekommt einen Wakeup-Parameter (Sender 0,
Signal 1), das Parameter-Event wird signalisiert. Folge: Frontier 9 -> 3
(`hid:USER`). Die 9 waren nur scheinbar: Der Gast-RomFS-Lookup schlaegt fehl
(err:f `cmd 0x0001`, `E0A046DB`), danach legt das Spiel absichtlich einen
Endlos-Halt ein (`LightEvent_Wait` auf ein Event auf dem Stack). Der alte
Arbiter liess diesen Halt per Timeout 0 sofort zurueckkehren.

### RomFS-Mount gelingt – Ursache ist die fehlende FS-Session (2026-11-24)

**Korrigierter Befund:** Nicht der Hash-Lookup scheitert. `fn_0027DE14`
(RomFS-Mount) liest Level-3-Header und alle vier Tabellen erfolgreich
(`[file] Read off=0x0/0x28/0x4C/0x160/0xAF4`, jeweils volle Laenge), prueft
`len == 0x28` und die Tabellensummen und initialisiert danach seine Struktur.
`fn_00212CD0`/`fn_00212E3C` (Verzeichnis-/Datei-Lookup) werden **nie
betreten** (`ARM_WATCH=212CD0/212E3C`: 0 Treffer); der Hash-Pfad ist also
nicht die Ursache des Halts.

**Wo der Fehler entsteht:** Im Erfolgspfad ruft der Mount bei `0x27E198`
`fn_001250F0 -> fn_0028EDCC`. Das ist der libctru-FS-Session-Zugriff: er
liest `[g_3D8D8C]` und ruft bei 0 `fn_0028F45C` mit `E0A046DB` auf (err:f).
`g_3D8D8C` wird ausschliesslich vom Konstruktor `fn_00381918` aus
`[0x39A0CC]` gesetzt; beide sind 0. `fn_00291C74` (das eigentliche
`fsInit`: `srvGetServiceHandle("fs:USER")` ueber `0x28EE00`, danach
`fn_00107A78` -> `[0x3D8D98+0x10]`) hat **keinen statischen Aufrufer und wird
im Boot nie ausgefuehrt** (`ARM_WATCH=291C74`: 0 Treffer).

**Beleg aus der Referenz:** Azahar oeffnet `fs:USER` per
`srv GetServiceHandle` bei `1.309988`, direkt nach der APT-Initialisierung
(`reports/azahar_trace.txt:340`). Im Port-Log fehlt dieser Request komplett;
die ersten `fs:USER`-Kommandos gehen mit Handle 0 hinaus und werden nur
deshalb beantwortet, weil `dispatch_ipc` nach Kommandoform routet
(Workaround §4). Auf echter Hardware muss die FS-Session vor dem Mount
existieren.

**Naechster Arbeitsvorrat:** ~~Herausfinden, warum der Boot die FS-Session-
Initialisierung (`fn_00291C74` bzw. deren Aufrufer) ueberspringt~~ –
**erledigt**: nicht der Aufrufer fehlte, sondern der Fallthrough nach dem
bedingten `bxne lr` in `fn_00291C64` (siehe oben, „FS-Session fehlte wegen
bedingtem `bx`“). `tools/fix_romfs_cycles.py` und
`tools/rebuild_romfs_hash.py` waren dafuer nachweislich nicht noetig und
sind entfernt.

### Interworking-Veneeren liefen als No-op – behoben 2026-11-24

**Befund:** `out/missing_calls.txt` sammelte 453 Aufrufe an `0x0029226D`
(von Block `0x00292264`) und Treffer an `0x00100373` (von `0x0024CCEC`).
Beides sind Linker-Stubs: `0x292264: add r12, pc, #1; bx r12` springt nach
`0x29226C` (Thumb), `0x24CCEC: ldr pc,[pc,#-4]` laedt das Literal
`0x00100373`. Die Ziele sind echte Thumb-Funktionen (%d/%u- bzw.
%x-Handler der Format-Ausgabe), wurden aber nie geseedet, weil sie mitten in
einer ARM-Funktion liegen bzw. nur ueber ein Literal erreichbar sind; der
Aufruf wurde zum No-op.

**Korrektur:** `tools/recomp/scan.py` verfolgt PC-relative Konstanten im
Block (`add/sub rN, pc, #imm`, `adr rN, #imm`) und liest das Literal
statischer `ldr pc, [pc, #imm]`-Stubs. Ein Ziel mit Bit 0 wird als eigene
Thumb-Funktion geseedet. Test:
`tools/selftest_tools.py::test_veneer_thumb_seed`.

**Folge:** `missing_calls.txt` ist im 40-s-Lauf leer; die beiden Funktionen
`fn_0029226C`/`fn_001006A0` bzw. `fn_00100372`/`fn_001004A0` werden
ausgefuehrt. Frontier bleibt 10/10; Draw-Trigger (0x22E/0x22F) kommen
weiterhin in keiner Liste vor. Beleg: `HLE_PICA_DIAG=1` (Register-Histogramm
je Liste, `docs/TOOLS.md`) zeigt fuer alle Listen nur Setup-Register
(Rasterizer, Texturen, TEV, Attribut-Loader 0x200..0x229, Shader-Uploads,
Finalize 0x010), keine 0x22E/0x22F und keine Kanal-Spruenge.

### Videodecoder: Handler dekodiert, Ringpuffer-Stall nach 41 Frames – offen 2026-11-24

**Behoben (Recompiler, eigene Ursache):** Beim Dekodieren ab dem 42.
Videoframe landeten Tabellenziele im No-op. Zwei Ursachen in
`tools/recomp/scan.py` (der 41-Frame-Stall selbst blieb danach bestehen,
siehe unten):

* Die Offset-Tabellen `0x16A958`/`0x16AED4` haben zwei Selbstverweis-Slots
  (Index 6/7 -> Tabellenbasis) vor den Handlern `0x16AA08`/`0x16AA20` bzw.
  `0x16AF84`/`0x16AF9C`. `offset_table_targets()` brach beim ersten
  Selbstverweis ab; er wird jetzt uebersprungen.
* Die Handler `0x16E70C`/`0x16E8BC`/`0x16E9E0`/`0x16EB68` werden ueber zwei
  Schritte adressiert (`add r4, pc, #hi; add r4, r4, #lo; movle pc, r4` in
  `0x16C390..0x16C530`). Der Scanner schrieb nur die PC-relative erste
  Addition in `const_reg` fort; die zweite Stufe wird jetzt ebenfalls
  fortgeschrieben.

Belege: `out/missing_calls.txt` (vorher) `0x16E70C` (192x), `0x16AF84`
(112x), `0x16E8BC` (88x), `0x16E9E0` (72x), `0x16AA20` (72x), `0x16AF9C`
(64x), `0x16EB68` (40x), `0x16AA08` (40x); nach dem Fix sind diese Ziele 0,
`ARM_WATCH` belegt ihre Ausfuehrung (z. B. `0x16E70C` 65 Treffer,
`0x16AF84` 78, `0x16E8BC` 98 in 5 s). Selbsttests:
`tools/selftest_tools.py` (Offset-Tabelle mit Selbstverweisen,
Zwei-Schritt-PC-Adresse), 19/19 gruen; `tests/mvp_selftest.py` 45/45.

**Weiter offen (Ursache des 41-Frame-Stalls):** Der Decoder-Thread 0 laeuft
nach 41 `StartConversion` in den Huffman-Loop, weil sein Eingabepuffer nur
Nullen liefert: `HLE_STALLDUMP` zeigt dauerhaft `pc=0x16E4A8/0x16E4B4`;
`ARM_WATCH=16E4A0` zeigt `r1=0x0C683Axx` (Ringpuffer) und der Bit-Refill
`0x16C568` liest dort 0. Der I/O-Thread `F89` hat anfangs nur 4x `0x4000`
Byte (64 KiB) gelesen (`off=0x379F1BE0..0x379FDBE0`); die naechsten
Datei-Reads (`0x37A05BE0...`) kommen erst nach dem Stall. Der Decoder laeuft
also ueber das Pufferende hinaus, statt auf Nachschub zu warten. Naechster
Schritt: die Ringpuffer-/Event-Wartestelle des Decoders (Thread 0) gegen
Azahar pruefen; der 1-ms-Poll (`svc 0x19 ClearEvent / 0x18 / 0x0A`) des
I/O-Threads ist das Gegenstueck. `HLE_PERFMAP_FILE` zeigt, dass der
I/O-Thread (thread 10) waehrend des Stalls 1,27 M Bloecke ausfuehrt, also
nicht vom kooperativen Scheduler ausgehungert wird; die Warte-/Bedingungs-
logik des Ringpuffers selbst ist falsch.

**Nebenbefund (Recompiler, offen):** `fn_0020E250` kehrt mit
`ldr pc,[sp],#0x14` (Post-Index-SP) zurueck. Der Emitter behandelt das als
dynamischen Sprung und loggt die Rueckkehradresse `0x0035649C` als
"unbekanntes call-Ziel" (576x). Fuer direkte `bl`-Aufrufe ist der No-op
wirkungslos (die C-Funktion kehrt danach zum Aufrufer zurueck), aber die
Semantik sollte wie `pop {pc}` als `return` emittiert werden. Ebenso werden
Return-Adressen aus `mov pc, rN` (z. B. `0x16C254`) als Missing-Call geloggt.

**Korrektur zur frueheren Annahme:** Y2R schreibt das RGB8-Bild (400x240,
`SetReceiving` size `0x46500`) in den **linearen Heap** (`0x15E3DE00`), nicht
in die VRAM-Framebuffer (`0x1F300000`/`0x1F346500`). Im 60-s-Lauf gibt es
weiterhin **keinen** GX-DisplayTransfer/MemoryFill und keine Draws; der
Bildschirm bleibt einfarbig. Der Videopfad braucht nach Y2R noch einen
Transfer/Render, der ohne den Ringpuffer-Fix nicht erreicht wird.

### PICA-Listen-Ausreisser (415-430 vs. 15.738 in 40 s): Host-Last, kein Gastfehler – 2026-11-24

**Befund:** Die Zahl der `[gpu] list`-Zeilen ist nur ein Mass fuer die
erreichte Frame-Rate; der Gastverlauf ist in allen Laeufen identisch. Nach dem
Boot (2 Einmal-Listen: `168EC6B0 size=7480 ops=6590`, `168B5820 size=250
ops=103`) wiederholt sich ein festes Frame-Set aus **14 Listen**: `1686E820/270
ops=108`, `1686EA90/1C0 86`, `1686EC50/A0 21`, `1686ECF0/1B0 84`, `1686EEA0/A0
21`, `1686EF40/150 72`, `1686F090/80 16` und dieselben sieben Groessen ab
`168B5820`. Alle `draws=0`, keine 0x22E/0x22F, keine Kanal-Spruenge. Jede Liste
wird von GX `0x05` FlushCacheRegions (gleiche Adresse/Groesse) begleitet und
per `gsp::Gpu 0x000C` (n=0) aus dem Shared-Memory-Kommandopuffer (GX `0x01`)
getriggert: 14 Listen + 14 Flushes und ~40 Trigger je Frame. Die Trigger kommen
aus zwei Gast-Threads (TLS-Kennung `F82`/`F88`), der GSP-Interrupt-Handler
(Gast-Thread 6, Wartepunkt LR `0x00142620`) wartet auf das Relay-Event 32.

**Belege** (`scratch/outlier/`, Wegwerfskripte):
* `m0.log` (2.047 Listen / 45 s) und `load1.log` (408 / 12 s) enthalten
  dasselbe Multiset aus 16 (addr,size,ops,draws)-Tupeln, dieselben
  14 Listen/Frame und 40-41 Trigger/Frame; kein Retry, kein Fehler-/Abbruch-
  pfad, keine doppelten oder fehlenden Listen. Nur die Frame-Zahl differiert.
* `repeat.py` (8x12 s, ohne Last): 7.573-7.701 Listen/12 s (~45 fps) mit dem
  aus HEAD `282dd7d` gebauten `out/port.exe`. Mit der vorherigen (aelteren)
  Binary waren es 268-317/12 s – die Beobachtung stammt also aus einem
  anderen Build/Hostzustand, nicht aus einem Gastpfad.
* `realload.py`: 12 s ohne Last **7.654** Listen; mit 8 CPU-Lastprozessen
  **408** Listen (2,4 fps). Die Host-Last reproduziert das beobachtete
  "4 von 5 Laeufen langsam"-Muster exakt.
* `HLE_YSTAT=1`: bei 42 fps lag der Gast-Lock nur 3,8 % der Zeit frei; die
  Frame-Zahl folgt der verfuegbaren Host-CPU, nicht einem Lock-Starvation-
  oder IPC-Wiederholungsmuster.

**Hardware/Azahar:** Der GSP-Kommandopuffer ist kein IPC; Azahar
(`gsp_gpu.cpp` `TriggerCmdReqQueue`) fuehrt die Kommandos aus, loggt sie aber
nicht, und `reports/azahar_trace.txt` endet bei 11 s (Y2R-Aufbau), also vor
dem Render-Loop – die Referenzspur enthaelt daher keine GX-Rate. Auf Hardware
ist der Loop VBlank-getaktet (60 Hz): 14 Listen/Frame sind ~840 Listen/s bzw.
~33.600 in 40 s. **15.738** (393/s ~ 28 fps) ist als Software-Render-Rate des
Ports plausibel, **415-430** (~0,75 fps) ist reine Host-Starvation. Es gibt
daher keinen Kernel-/GSP-Fehler zu beheben; die Semantik bleibt unveraendert.

**Offen (nicht geaendert):** `guest_lock_reacquire()` weckt mit `Sleep(1)`
(auch mit `timeBeginPeriod(1)` ~1,5 ms), was jedem blockierten SVC-Wait eine
Latency-Untergrenze gibt und den Port empfindlich gegen Host-Scheduling macht.
Ein Umbau auf eine echte Handoff-Synchronisation waere moeglich, beruehrt aber
den kooperativen Scheduler und wurde ohne Not (Boot ist 10/10) nicht gemacht.

**Diagnose:** neuer Schalter `HLE_YSTAT=1` (`docs/TOOLS.md`) zaehlt Yields,
Wandzeit in den Lock-Abgaben und die Zeit, in der der Gast-Lock frei lag –
reine Zaehler, kein Gastzustand.

### 41-Frame-Stall: Videodaten sind vollstaendig geladen, Decoder liest ueber das Paketende – Befund 2026-11-24

**Korrektur der Auftragsannahme (belegt):** Der I/O-Thread hungert nicht und
die Datei-Reads fehlen nicht. Im 30-s-Frontier-Lauf liegen vor der 41.
`StartConversion` (Zeile 3612) bereits 87 `[file] Read`-Aufrufe; die
komplette `m999.moflex` wurde in neun 64-KiB-Happen
(`0x26B10510` … `0x26BA0510`, inkl. 0x12A4 am Ende) nach `0x0D0007C0`
am Stueck geladen. Die `0x4000`-Reads von `th=F89` sind die **Audiospur**
(jedem Read folgen zwei `dsp::DSP 0x0013`-Pipe-Writes), nicht das Video. Alle
87 Reads liefern `done == len` (kein Teilread). Ein Datei-/fs-Fehler ist damit
als Ursache des Decoder-Stalls ausgeschlossen.

**Neue Belege mit `HLE_STALLDUMP` (Register/Stack) und `ARM_MEMWATCH`
(nur Diagnose, kein Gastzustand):**

* Register des haengenden Threads: `r0=0C682378`, `r1=0CABFDFA`, `r2=3..9`,
  `r3=0`, `sp=1667FC48`; Stack enthaelt die Decoder-Rueckkehrkette
  `0016C520 → 0016C400/0016C41C → 0016BFCC → 0016A7B0`, darunter
  `00390338` (`0x3902E0` Queue-Push) und `0033C9FC` (`0x33C9E4`).
* `ARM_MEMWATCH=0CABFDF8` und `=0CABFE00`: **0 Schreibzugriffe**. Der
  Bitstromzeiger `r1` liegt also hinter dem beschriebenen Bereich; ab `r1`
  stehen Nullen, `r1-0x200` enthaelt noch gueltige Koeffizienten
  (`FFFFFFFF 7FFF0000 FFFFFFFF …`). Der Decoder liest ueber das Paketende
  hinaus.
* `ARM_WATCH=16BD1C` zeigt: die Codebook-Auswahl laeuft (2x) und schreibt
  eine gueltige statische Huffman-Tabelle (`0x003C1E2C` bzw. `0x003BFD2C`)
  nach `[obj+0x3b8]`. `ARM_MEMWATCH=0C682730` (=`obj+0x3b8`) belegt danach
  zusaetzliche Stores von `003BFD2C` aus `0x16BC1C` (normale Paketauswahl).
* Erst der Huffman-Loop selbst ueberschreibt `[obj+0x3b8]` mit 0: bei
  `r3=0` liefert die Tabelle `0x0001`, der Pfad setzt `r6=0, r5=0`, und
  `str r7,[obj+0x1b8+r5*4]` schreibt mit dem aus den Nullbytes
  gewonnenen `r5=0x80` auf `obj+0x3b8`. Der bei der Stichprobe beobachtete
  Null-Zeiger ist also **Folge**, nicht Ursache des Haengers.
* Kernel-Primitive geprueft: `ArbitrateAddress` Typ 1 vergleicht vorzeichen-
  richtig (`mem=-1 < 0` blockiert), Events (OneShot/Sticky/Pulse-Acquire)
  und Semaphoren entsprechen Azahar; kein Kernel-Fehler ist als Ursache
  belegt.

**Naechster Schritt:** Die Kopierstelle Paket→Decode-Puffer identifizieren
(Byte-Kopien aus `pc=0x20D2CC` bzw. `0x358FA8`) und Laenge/Quelle gegen den
Moflex-Paketkopf pruefen. Faellt die Kopierlaenge zu kurz aus oder laeuft der
Praesentations-Event dem Kopierer davon, liegt die Ursache dort (Event-Kette
`0x218xxx`/`0x3902E0`); andernfalls ist es der parallele Decoder-Differenztest.

### Decoder wird pro 96-Byte-Chunk gerufen; der 64. Aufruf (nach Frame 41) liest 6,4 KiB – Befund 2026-11-24

**Befund (nur Diagnose, kein Gastzustand):** Der Haenger ist der **64.** Aufruf
des Frame-Decoders `0x0016A500`, und zwar unmittelbar **nach der 41.
`StartConversion`** (Korrelation mit `HLE_LOG=1` + `ARM_WATCH=0x0016A500`:
24 Aufrufe vor Frame 1, danach 1 Aufruf je `StartConversion`, der 64. direkt
danach). Bis Frame 41 stirbt der Decoder also nicht an einem fehlenden Pfad.

**Neue Belege:**

* `[mw] 0C682378 <- ...` (`ARM_MEMWATCH=0C682378`, Schreiber `pc=0x17A6F8` =
  `str r1,[r5]` in `fn_0017A6A0`) zeigt die Bitstrom-Zeiger je Decoderaufruf:
  `0CABDF28`, dann in **0x60-Schritten** `0CABE0E4, 0CABE144, ... 0CABE924,
  0CABE984, ... 0CABEE04`, Umlauf auf `0CABDF80 ... 0CABE820`, zuletzt
  `0CABDF28` und `0CABE504`. Der Ring wird also **in 96-Byte-Chunks** an den
  Decoder uebergeben (0x17A6A0 liest dafuer 6 Referenz-Eintraege, ein
  16-Bit-Kopfflag `[r1]&0x8000` und ruft je nach `[r4+0x18]` (0x100/0x200/
  0x400) einen Handler ueber `blx`; der Haenger sitzt im 0x200-Zweig,
  Rueckkehr `lr=0x0017A788`).
* Der Stalling-Aufruf startet bei `[obj+0] = 0x0CABE504` und steht bei
  `r1 = 0x0CABFDFA`, d. h. er hat **0x18F6 = 6390 Byte** verbraucht, waehrend
  alle vorherigen Chunks 0x60 Byte auseinanderlagen. `[obj+4]=0x190` (400),
  `[obj+8]=0xF0` (240) bleiben korrekt; `[obj+0xc..0x1c]` zeigen gueltige
  Referenzebenen (`0C8FA7C8, 0C8CD5A8, 0C8A0388, 0C873168, 0C845F48`).
  Offenbar beginnt bei diesem Chunk ein **Intra-Frame** (Keyframe), dessen
  Daten der Ring noch nicht bereitgestellt hat.
* Der Ring-Schreiber ist `fn_00358958` (Struktur `0x0C680A20`, Datenpuffer
  u. a. `0x0CAB...`, Quelle `0x0D0007C0` = vollstaendig gelesener Moflex);
  sein Kopierpfad laeuft ueber `pc=0x358FA8` (Byte-Loop) bzw. `0x20D2CC`
  (Byte-Kopie, im `0x0CABF000`-Bereich mit 0xFF gefuellt). Der Ringpuffer ist
  also ein echter Produzent/Verbraucher-Ring, kein fehlender Datei-Read.
* `HLE_PERFMAP_FILE` (12 s, 900 Host-Frames): Thread 0 95,2 M Bloecke, davon
  `0x16E4B4`/`0x16E4A8` je 45,8 M (Huffman-Loop); Thread 2 8,7 M (u. a.
  `0x12B9E8`/`0x12B968` = zweiter Bitstrom-Decoder, laeuft weiter); Thread 10
  3,7 M (`0x14FF9C`/`0x14FFF0` = GSP). Die Produzenten-Threads sind also
  **nicht** ausgehungert; es fehlt die Freigabe/Kopfreaktion auf den
  96-Byte-Chunk des Decoders.

**Naechster Schritt (praezisiert):** Im Chunk-Geber `fn_0017A6A0` und im
Ring-Schreiber `fn_00358958` die Zaehler „geschrieben/gelesen“ und das
Freigabe-Event beobachten (`ARM_MEMWATCHRANGE` auf die Ringstruktur
`0x0C680A20` bzw. den Chunk-Index in `[r4+0xc]`/`[r4+8]`); pruefen, ob der
Decoder beim Intra-Frame mehr als einen 96-Byte-Chunk anfordern muss und
welches Event/Semaphor den Kopierer weckt. Erst danach ist eine
Kernel-/Service-Korrektur belegbar.

### Y2R-Sendeparameter: `gap` ist `-unit` statt `512-unit` – offen 2026-11-24

**Befund (IPC-Parameter, nicht Sequenz):** Die Dienstfolge ist weiterhin
kanonisch gleich (`trace-diff`), und der Y2R-Framepfad ist vorhanden
(41 vollstaendige Zyklen). Die erste inhaltliche Abweichung steckt in den
Parametern von `SetSendingY/U/V`:

| Kommando | Referenz `transfer_stride` | Port `[y2r] ... gap` |
|---|---|---|
| SetSendingY (0x0010) | 112 (0x70) | 65136 (0xFE70) |
| SetSendingU/V (0x0011/0x0012) | 312 (0x138) | 65336 (0xFF38) |
| SetReceiving (0x0018) | 2688 (0xA80) | 2688 (0xA80), stimmt |

In der Referenz ist `unit + gap` immer 512 (400+112, 200+312). Der Port
uebergibt `gap = -unit` (als int16: 0xFE70 = -400, 0xFF38 = -200), also
`unit + gap = 0`.

**Belege:**
* `grep "\[y2r\] cmd 0x0010" out/logs/frontier.txt` →
  `gap=65136`; `reports/azahar_trace.txt:477` → `transfer_stride=112`.
* `ARM_WATCH=00218660` (Einstieg des Presenters): `r2=0x190` (400),
  `r3=0xF0` (240), `lr=0x00312B90`. Der Stride-Parameter steht im Stack
  und stammt aus `[r5+0xc]` (`vldr s1,[r5,#0xc]` →
  `vstr s1,[sp,#0xc]` in `fn_0031282C`). Der Port liest dort 0; erwartet
  ist 512.
* Im Presenter `fn_00218660` entsteht `gap` als `sb - r5`
  (`0x218754: sub r0, sb, r5`); das Register `sb` (r9) wird aus dem
  Stackargument geladen (`ldr sb,[sp,#0x44]`). Mit `sb = 0` ergibt das
  exakt `-unit`.

**Einordnung:** Der Wert entsteht im Gast-/Presenter-Pfad
(Videoframe-Metadaten), nicht in einem Kernel- oder Dienst-Handler. Kein
Workaround eingebaut; die Korrektur gehoert in den Decoder-/Frame-Strang.
Fuer M4 relevant, sobald das Video wieder vollstaendig dekodiert wird.
### Mobiclip-Decoder: Film haengt nach 41 Bildern – behoben 2026-11-24

**Befund:** Der Film `m999.moflex` hat 520 Bilder (unabhaengiger Parser
`tools/cosim/moflex.py`); Bild 2-62 sind identische 94-Byte-Bilder, Bild 63/64
das erste mit Inhalt (1.498/1.108 Byte). Der Decoder las beim ersten
Inhaltsbild 6.390 statt 1.108 Byte und geriet aus dem Bittakt. Unicorn auf
den Originalbytes ab dem Port-Speicherabbild las genau 1.108 Byte;
`tools/cosim/lockstep.py` zeigte die Abweichungen Block fuer Block.

**Ursachen (alle im Recompiler, generisch behoben, je mit Testfall):**
1. Pseudo-Aufruf `add lr, pc, #k; [push {.., lr}]; b<cond> ziel` mit LR auf
   die Fortsetzung wurde als Sprung uebersetzt; das `pop {.., pc}` des Ziels
   beendete die C-Funktion (`pseudo_aufruf_b_mit_lr`, `bedingter_pseudo_aufruf`).
2. Logische S-Befehle mit rotiertem Immediate setzten C nicht auf Bit 31
   (`tst r3, #0x80000000`, `tst_rotiertes_immediate_carry`).
3. Berechnete Aufrufe `add lr, pc, #k; add pc, rX, rY`: die LR-Fortsetzung
   war kein Blockanfang, lokale Ziele wurden per goto statt als Aufruf
   ausgefuehrt (`berechneter_aufruf_lr_fortsetzung`).
4. Zweistufige LR-Bildung `add lr, pc, #hi; add lr, lr, #lo`
   (`berechneter_aufruf_lr_zweischritt`).

5. ARM-Switch `ldr<cond> pc, [pc, rN, lsl #2]; b default; .word ...`: der
   Default-Zweig bei +4 wurde nie dekodiert (Fallthrough hinter die
   Tabelle). Betroffen war der Erfolgspfad von 0x38FBB0, der die
   Y2R-Bildbeschreibung per `vstmia` kopiert (`arm_switch_default_zweig`).

**Ergebnis:** 520/520 y2r `StartConversion` (vorher 41), Quellparameter wie
in der Azahar-Referenz (Gap 112/312). Der Y2R-Ausgabepuffer enthaelt die
echten Filmbilder (8x8-Kacheln, RGB24, 512x240). Offen: das Spiel zeichnet
die Textur noch nicht (keine PICA-Draws), der Bildschirm bleibt einfarbig.

### Draw-Gate 0x27B34C: Spielzustand, kein Kernel-/Dienstfehler – ausgeschlossen 2026-11-24

**Hypothese (Auftrag):** Der Renderer erreicht seine Draw-Aufrufe nicht, weil
eine Bedingung aus Kernel/Diensten falsch ist.

**Ergebnis: Hypothese widerlegt.** Kein Dienst- oder Kernelwert blockiert den
Draw; die Draw-Anforderung wird vom Spiel selbst nie erzeugt. Belege:

* **Sechs Aufrufer, keiner laeuft.** `HLE_PERFMAP_FILE` (1200 Frames, 20 s)
  zaehlt fuer 0x27B34C **0** Bloecke. Auch die Einstiege der Wrapper
  0x11AD4C/0x250FE8/0x256E40/0x27B6CC/0x2812EC sind 0; nur 0x11CAC4 laeuft
  (2679x). `ARM_WATCH=0x11CAC4` zeigt `r2=0` in allen Treffern
  (`lr=0x28C7B0`/`0x28CE74`); die beiden Aufrufer setzen `mov r2,#0`
  (0x28C79C, 0x28CE60). Damit nimmt 0x11CAC4 immer den Zweig nach
  0x11CD90 (Farb-/Alpha-Pfad) und ueberspringt den Draw-Aufruf bei 0x11CCDC.
* **Gate der Draw-Funktion:** 0x27B34C kehrt bei 0x27B3A8 mit 0 zurueck,
  wenn `[0x408808+0xb4c] == 0`. Perfmap: 0x27B844 wird 5496x gerufen und
  verlaesst die Funktion immer ueber 0x27B990 (Rueckgabe `[obj+0xb4c]` = 0);
  der Pfad ueber 0x27B958/0x27B964 (Rueckgabe 1) laeuft **nie**.
* **Was 0xb4c ist:** 0x408808 ist das Grafikobjekt des Spiels (Vtable
  0x3C5EC4, String „lxpCOpenGLES“ ab +0x20). +0xd6c zeigt auf einen der
  beiden eingebetteten Render-Kontexte (+0x214/+0x458, doppelt gepuffert,
  gesetzt in 0x1ED660). 0x13657C uebernimmt `[ctx+0x24]` (aktiver Layer)
  nach `[renderer+0xb4c]`, aber nur wenn `[renderer+0x2b44] & 1` gesetzt ist.
* **Der Layer ist immer leer:** `ARM_MEMWATCH=409354` (renderer+0xb4c):
  3 Schreibzugriffe, alle 0 (Init/Memset). `ARM_MEMWATCH=408A40` und
  `=408C84` (ctx0/ctx1+0x24): alle Schreibzugriffe 0, auch ueber 900 Frames
  (15 s). Das Spiel aktiviert also nie einen Render-Layer; die Kontexte
  werden von der Engine selbst (Aufrufe aus 0x26A87C/0x1ED6xx) zurueckgesetzt.
* **Dienste sauber:** kein `err:f`, alle 10 Dienste 10/10; einzige
  `UNIMPLEMENTIERT`-Zeilen sind `boss:U 0x000E` (GetTaskIdList) und
  `0x0016` (ReceiveProperty), die Azahar fuer einen Titel ohne SpotPass-Task
  ebenfalls ohne Nebenwirkung beantwortet. GSP nimmt alle Kommandos an.
* **Kein Parser-Loch:** ein eigener Parser der Rohdumps aus
  `HLE_PICA_DIAG=1` (`scratch/pica/list_*.bin`) findet in **keiner** Liste
  0x22E/0x22F/0x23C/0x23D. Die 14 Listen je Frame sind reiner Zustand
  (Rasterizer, Viewport, TEV, Attribute 0x201..0x203, Topologie 0x25E,
  Shader 0x2C0/0x2CB) – die Draw-Anforderung fehlt im Gastcode, nicht im
  PICA-Parser des Ports.
* **Das Spiel laeuft weiter:** nach 520/520 Y2R-Frames liest es ~300 KiB
  neue Assets direkt aus dem RomFS (Reads ab `0x1A705B10`, `0x1B3AEC`, …)
  und liest `head.lt6` erneut; der Hauptthread zykelt danach normal durch
  Mutex 0x40F120 (Schreibfolge 1/-1, keine Blockade). Ein haengender
  Kernel-Wait ist damit ebenfalls ausgeschlossen.

**Naechster Schritt:** Im Spiel-Renderer ansetzen: warum aktiviert die
Szene keinen Layer (0x13657C verlangt `[renderer+0x2b44]&1` und einen
gesetzten `[ctx+0x24]`)? Das ist Szenen-/Zustandslogik des Spiels
(0x13657C, 0x26A84C, 0x1ED65C), keine HLE-Frage. Die Filmtextur selbst
liegt korrekt in `0x15E3DE00` vor.

### Renderer-Ressourcen-Registry 0x4088A8: Schreiber fehlt, Schluessel sind CRC32-Namen – Befund 2026-11-24

**Aufbau (statisch + Laufzeitdump):** `0x4088A8` (= `0x408808+0xa0`,
Grafikobjekt) ist eine Ressourcen-Gruppe mit vtable `0x3C6010`, konstruiert
von `0x1FB3C4` ueber `0x238D1C`. Sie enthaelt zehn Paare aus Objekt-Pool und
Zeiger-Tabelle; jeder Container ist `0xC` Byte (`[0]=Feld`, `[4]=Elementgroesse`,
`[8]=Anzahl u16`, `[0xa]=Kapazitaet u16`). Die vom Frame-Treiber benutzte
Tabelle liegt bei `+0x70` = `0x408918` (Elemente 4-Byte-Zeiger), Anzahl
`0x408920`.

**Schluessel:** `fn_0027BC50(reg, key)` binaersucht `[reg+0x70]`/`[reg+0x78]`
mit Comparator `fn_00134900` (vergleicht `entry+0xc`); bei Misserfolg wird in
`[reg+0x10]` (Parent) rekursiert. Der String-Weg ist `fn_002461D8(reg, name)`:
`fn_00288500` ist CRC32 (zlib-Polynom, `~state`), danach dieselbe Suche.
**Beleg:** `0x2D2F398D` = CRC32(`"#FIX_CLR"`). Weitere Namen im Image:
`"#FIX_IMG"`, `"#FIX_FNT"`, `"#FIX_2D"`, `"#LT6_SES"`. Die Eintraege sind
also benannte, eingebaute Renderer-Ressourcen (Fonts/Bilder/Passes), keine
Szenen-Hashes.

**Kein Schreiber im Boot:** `ARM_MEMWATCH=408920`/`=408918` (30 s und 90 s)
zeigt ausschliesslich die BSS-Nullung (`pc=0x100030`) und die
Konstruktor-Initialisierung (`fn_00288398`, `lr=0x238DB8`: Groesse 4, Anzahl
0). Ein Dump `0x408800..0x408B00` am Laufende zeigt **alle** Container der
Gruppe leer. Die 5 Aufrufer von `fn_0025618C` (Kommando-Verarbeiter
`0x2B9368/0x2DA160/0x2DA380/0x2DAA68/0x2EF3DC`) geben `0x4088A8` nur als
Argument an `fn_00249EB8` (Archiv-/Font-Load) bzw. `fn_00277B84` weiter.

**Korrektur des frueheren Befunds:** `fn_0026F924` ist **kein**
Resource-Lookup, sondern eine Archiv-Magic-Pruefung: es vergleicht `[obj]`
mit `[0x3D9458]`=`"XPCK"` bzw. `[0x3D945C]`=`"XFSP"` (Laufzeitdump der
Globals). Alle fuenf beobachteten `fn_00277B84`-Aufrufe kommen **ueber** die
Magiepruefung und ueber `fn_0026F8BC`/`0x26B904` hinaus: der Ausgang
`0x277DB0` (= Abbruch vor dem Oeffnen) hat in 15 s **0** Treffer, der
Erfolgsblock `0x277C14` **5**. Die geoeffneten Archiveintraege haben die
Groessen `0xA58` (das ist `seq/menu_logo.xq`), `0x4F8`, `0x8B0`, `0x139C`,
`0x1A38`. Der fruehere Befund „bricht nach dem Ressourcen-Lookup ab“ ist
damit widerlegt; der Pfad laeuft weiter und endet erst spaeter.
`fn_00249EB8` (Font-Load) verlangt vor dem Laden, dass `"#FIX_FNT"` bereits
in der Gruppe steht – die eingebauten Ressourcen muessen also vorher von
einer anderen Stelle erzeugt werden.

**RomFS-Lookup (Pruefung der entfernten Patches):** Der NotFound-Fall
(`0x212E24`, `r0=0xC880446F`) ist der Verzeichnis-/Datei-Lookup des Namens
`"stream"` (UTF-16, Laenge 12) in `lt6/snd/uk`. Der Pfad-Walker erkennt
genau diesen Code (Modul 17, Desc 0x6F) bei `0x2055A0` als "weiter
probieren"; anschliessend werden Dateien aus `.../stream/` gelesen
(`MOV_999.dspadpcm.bcstm`). Der Fehlschlag ist normales Probing, kein
Boot-Blocker; ein Wiederauftreten der Ursachen von `0x212F3C`/`0x399244` ist
nicht belegt.

**90-s-Lauf (Vollspeed):** 520/520 `StartConversion` (Film komplett),
42.736 PICA-Listen, 7.226 `SetBufferSwap`, `seq/prog_common.xq` und
`seq/menu_title.xq` werden aus `lt6_a.fa` gelesen (`0x48EE24` bzw.
`0x473AE0`) – die Skriptebene laeuft, die Ressourcen-Gruppe bleibt trotzdem
leer.

**Naechster Schritt:** Die Erzeuger der eingebauten Ressourcen
(`"#FIX_CLR"`, `"#FIX_IMG"`, `"#FIX_FNT"`, ...) suchen; Kandidat ist der
Ressourcen-Set-Manager um `0x1FBxxx`/`0x23Dxxx` (vtable `0x3C6010`). Erst
wenn ein *Schreibversuch* auf `0x408918`/`0x408920` existiert und
fehlschlaegt, ist eine Runtime-Korrektur belegbar.
### Erster selbst gerenderter Frame (M3/M4) – 2026-11-24

Drei weitere Fehler verhinderten nach dem Film jedes Bild:
1. **Recompiler, Thumb `add rD, pc`** (Register-Form): PC wurde wie bei
   `adr` auf 4 Byte ausgerichtet; richtig ist Adresse + 4. Im `printf` des
   Spiels (0x1000FA) zeigte die Formattabelle 2 Byte zu frueh, `%03d` wurde
   mit Leerzeichen formatiert (Dateiname `15_   999.xs`), Szenen-/Layout-
   Ressourcen wurden nicht gefunden, die Renderer-Registry blieb leer und
   das Draw-Gate (0x27B34C) geschlossen. Gefunden per `tools/cosim` an
   fn_00277B84 (Test `thumb_add_reg_pc_unausgerichtet`).
2. **PICA Attribut-Loader/Indexpuffer**: Basis (0x200) wurde vor dem
   Addieren des Loader- bzw. Index-Offsets in Gastadressen umgerechnet; die
   Basis liegt im VRAM-Fenster, die Daten per Offset im FCRAM. Jetzt wird
   die volle physische Adresse umgerechnet.
3. **PICA Scissor**: 0x065 Modus, 0x066 Position, 0x067 Ausdehnung (Azahar
   regs_internal.cpp); der Port las x1/y1 aus 0x065 -> 1x1-Rechteck.

**Ergebnis:** M3 = ja (Draws), M4 = ja: `reports/port_intro_frame300.png`
(Intro-Film, oberer Bildschirm), `reports/port_frame1800.png`.
