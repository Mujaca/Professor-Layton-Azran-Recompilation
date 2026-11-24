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
| – | generierter Code | unbekannte Aufrufziele = No-op (geloggt in `out/missing_calls.txt`) |
| – | `arm_mem_read/write` | Zugriffe außerhalb des Speichers liefern 0 statt Abbruch (`ARM_STRICT=1` bricht ab) |
| – | fs `OpenFile`, `file_command` | Sentinel-Dateislot und Erfolg auch bei fehlgeschlagenem Open; `romfs_image.bin` mit Offset +0x1000 |

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
unteren Screen auf `HleHidInput` ab (`runtime/platform/hid_ring.h`); das
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
320x240). `runtime/platform/gsp_fb.h` bildet `GPU::SetBufferSwap` nach
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

Diese Liste stammt aus dem Review der Nachtarbeit (2026-11-24) gegen Azahar.

`wa-kernel-signal-all` ist **entfernt**: `vblank_thread` flutet keine Handles
mehr und postet keine Arbiter-Wörter; auch `WaitSynchronizationN` signalisiert
keine Handles mehr pauschal. Echte Quellen: GSP meldet PDC0/PDC1 je
VBlank (Host-Takt 16 ms) über die InterruptRelayQueue an das registrierte
Event; P3D/PPF/PSC0/PSC1/DMA werden von den GX-Kommandos ausgelöst
(`gsp_process_cmdqueue`). Beleg: Frontier 4 → 9 Dienste (Divergenz boss:U)
mit identischen 23 Patches.

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
(InterruptType, Pipe); `cmd 0x0007 SetSemaphore` signalisiert danach die
Pipe-Ereignisse (Azahar `DSP_DSP::SignalInterrupt`). Vorher blieb der
Hauptthread direkt nach `SetSemaphore` auf seinem eigenen Event (Handle 35,
`HLE_KLOG`) stehen; jetzt laufen `ReadPipeIfPossible`/`ConvertProcessAddress`
weiter. **Pipe-Inhalt (2026-11-24):** `WriteProcessPipe(Audio, Init)` liefert
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
