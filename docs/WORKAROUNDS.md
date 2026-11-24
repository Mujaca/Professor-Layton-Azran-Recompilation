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

Offen: `tools/fix_romfs_cycles.py` und `tools/rebuild_romfs_hash.py`
verändern weiterhin das RomFS-Image, das der Port ausliefert. Auch das ist
ein Workaround und sollte als nächstes durch korrekte IVFC-/Hash-Semantik
oder ein unverändertes Image ersetzt werden.

## 2. Block-Hooks im Recompiler – `tools/workarounds/block_hooks.json` (0)

**Leer.** Entfernt wurden `0x1079D0`/`0x107A30` (IPC-Reply-Größe),
`0x11A818` (Busy-Wait), `0x399244` (Stack-Slot) und der Hook nach
`bl 0x287DD4` (Allocator-Ersatz). Beleg: Build ohne alle Hooks, Frontier
bleibt 9/10 (boss:U). Die Kernel-Synchronisation (`runtime/kernel/hle.c`)
und die GSP-Interrupts liefern inzwischen die echten Werte/Zustände, die
die Hooks vorher ersetzt haben.

Früher entfernt: `0x12E1E4`/`0x12E1F0` (`arm_mutex_bootstrap`) – `ARM_WATCH`
zeigte im gesamten Boot 0 Treffer (toter Code).

## 3. Textersetzungen im generierten C – `tools/workarounds/gen_patches.py` (1)

| Funktion | Wirkung | Ursache (Verdacht) |
|---|---|---|
| fn_0028EE00 | srv-Namenslänge 8 → 9 akzeptieren | Spiel schickt `strlen+1`: Recompiler (strlen) oder srv-Layout |

Beleg: Mit leerem `gen_patches` fällt Frontier auf **6/10**, erste
Divergenz `gsp::Gpu`; der Check `cmp r2,#8` in `fn_0028EE00` (0x28EE14)
weist die Namenslänge 9 ab. `gsp::Gpu`/`dsp::DSP`/`hid:USER` sind genau
8 Zeichen. Die Aufrufer berechnen `r2` über `fn_0028F5E8` (strlen,
0x12F79C und 0x28E9D8); die Wortschleife nutzt `uqsub8` und ist in der
Runtime korrekt implementiert – als nächstes muss isoliert geprüft werden,
warum dort 9 statt 8 ankommt (Kandidat: `ldr`-/`str`-Semantik oder
Literal-Pool in `fn_0028F5E8`).

Die übrigen vier Ersetzungen (fn_001187F8, fn_00100F98, fn_001ED64C,
fn_001B9938) sind entfernt: Build ohne sie bleibt Frontier 9/10 (boss:U).

## 4. Runtime – `rg "WORKAROUND(" runtime`

| Kennung | Ort | Wirkung |
|---|---|---|
| – | `dispatch_ipc` | leitet Kommandos nach Form statt nach Handle um (APT/cfg/fs) |
| – | generierter Code | unbekannte Aufrufziele = No-op (geloggt in `out/missing_calls.txt`) |
| – | `arm_mem_read/write` | Zugriffe außerhalb des Speichers liefern 0 statt Abbruch (`ARM_STRICT=1` bricht ab) |
| – | Y2R `GetTransferEndEvent`, DSP `GetSemaphoreEventHandle` | Events vorab signalisiert; Y2R `IsFinished*` immer wahr (Azahar: unsignalisiert, Signal bei echter Fertigmeldung) |
| – | DSP `ReadPipeIfPossible` | schreibt abwechselnd 1/0 in den Gastpuffer statt echter Pipe-Daten |
| – | srv `GetServiceHandle`, `generic_command` | unbekannte Dienste/Kommandos liefern Erfolg |
| – | fs `OpenFile`, `file_command` | Sentinel-Dateislot und Erfolg auch bei fehlgeschlagenem Open; `romfs_image.bin` mit Offset +0x1000 |
| – | `SleepThread` | Schlafzeit auf 50 ms gedeckelt |
| – | `gpu_pica_virt_to_phys` | Adressen außerhalb VRAM/Linear-Heap bleiben unverändert (der Port hat keine MMU/Seitentabelle); echtes GSP lehnt sie ab |
| – | `p_draw_arrays` | Ohne hochgeladenes Vertex-Programm (0x2CB/0x2CC) fällt der Draw auf die alte Heuristik zurück (Position = Attribut 0, UV = Attribut 1). Mit Programm läuft der Azahar-Interpreter. |
| – | `p_process_tri` | Rasterizer nach Azahar: Sutherland-Hodgman-Clipping (7 Ebenen + 0x47/0x48..0x4B), Viewport 0x41/0x43/0x68, Perspective-Divide, 12.4-Fixed-Point-Coverage + Top-Left-Bias, perspektivkorrekte Attribute, TEV 0x0C0..0x0FD inkl. Combiner-Buffer, Texturen 0x080..0x09F (alle 14 Formate inkl. ETC1/ETC1A4, 8x8-Morton, Wrap/Border/Filter, drei Einheiten), Fog 0xE0/0xE1/0xE6/0xE8..0xEF (LUT-Upload mit Offset-Inkrement, Flip, Index/Fraktion, vor dem Depth-Test), Output-Merger 0x100..0x107 (Alpha-Blend, LogicOp, Alpha-Test, Kanalmasken) und Farb-/Tiefenziel 0x11B..0x11E (RGBA8/RGB8/RGB565/RGB5A1/RGBA4 in Azahar-Byte-Reihenfolge, D16/D24/D24S8, Depth-/Stencil-Test, 8x8-Morton). **Noch offen:** Lighting, Mipmaps, Proctex (Einheit 3), Cube-/Shadow-/Projection-Texturtypen auf Einheit 0; ohne konfiguriertes Farbziel bleibt der lineare Port-Pfad (RGB8) als Diagnose-Ausgabe. |
| – | cfg `GetConfig` | unbekannte Blöcke liefern Erfolg mit Nullen (Azahar: Fehler) |

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

**Offen (Stand Kernel-Sync-Paket):** Der Fehlerpfad
`fn_001028A0 -> fn_0028E410 -> fn_00119D34` (`g_os+9 = 1`) wird weiterhin
ausgeloest; `err:f` meldet `E0A046DA/DB` (Modul FS, Summary InvalidState,
Desc 0x2DA/0x2DB) mit `site=00212FD0`. Das ist der bekannte
RomFS-/Pfadparser-Pfad (Code-Patches um 0x212F3C/0x399244), **nicht** mehr
der Kernel: `services` ist inzwischen 9/10 (Divergenz `boss:U`).

Der Hauptthread bleibt nach `dsp::DSP cmd 0x0007 (SetSemaphore)` stehen: er
wartet auf ein eigenes Event (Handle 35, `svcCreateEvent` direkt nach
`dsp cmd 0x0011 LoadComponent`), das er zuvor per `dsp cmd 0x0015
RegisterInterruptEvents` an den DSP gebunden hat. Der Port hat keinen
Audio-/DSP-Kern, der Pipe-Interrupts liefert; Azahar signalisiert diese im
HLE-Audio (`DspHle`) beim Pipe-Processing. Naechster Arbeitsvorrat ist
daher DSP-Interrupt-Zustellung, nicht Kernel-Synchronisation.

### Arbiter korrekt, Boot stoppt am Gast-RomFS-Lookup (2026-11-24)

`svcArbitrateAddress` folgt jetzt Azahar (1/2 ohne Timeout, 3/4 mit Timeout
und immer `ResultTimeout`, Decrement nur bei 2/4). Fehlende Weckquelle war
APT: das erste registrierte Applet bekommt einen Wakeup-Parameter (Sender 0,
Signal 1), das Parameter-Event wird signalisiert. Folge: Frontier 9 -> 3
(`hid:USER`). Die 9 waren nur scheinbar: Der Gast-RomFS-Lookup schlaegt fehl
(err:f `cmd 0x0001`, `E0A046DB`), danach legt das Spiel absichtlich einen
Endlos-Halt ein (`LightEvent_Wait` auf ein Event auf dem Stack). Der alte
Arbiter liess diesen Halt per Timeout 0 sofort zurueckkehren. Naechste
Ursache: Lookup in `fn_00212E3C` (Vergleich bei 0x212F3C, Namensvergleich
`fn_0021077C`) bzw. das RomFS-Image (`tools/fix_romfs_cycles.py`).
