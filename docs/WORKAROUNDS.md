# WORKAROUNDS – technische Schuld

Alles, was vom **unveränderten Spielcode auf korrekter 3DS-Semantik** abweicht.
Ziel ist eine leere Liste (Meilenstein M8). Neue Einträge nur mit Begründung
und vermuteter Ursache. Jeder entfernte Eintrag ist echter Fortschritt.

Anzeigen/abbauen:

```powershell
python tools/build.py --pristine                       # ohne Code-Patches bauen
python tools/workarounds/apply.py --skip 0x003CBEFC    # einzelnen Patch auslassen
rg "WORKAROUND\(" runtime                              # Runtime-Workarounds
```

## 1. Patches am Spielcode – `tools/workarounds/code_patches.json` (23)

Werden vor dem Recompilieren auf `code.bin` angewendet. Gruppiert nach
vermuteter Ursache:

| Ursache (Verdacht) | Adressen | Quelle |
|---|---|---|
| **Locale/`mblen` liefert -1** (ctype-/TLS-Init oder Recompiler) | 0x13023C, 0x130270, 0x13028C, 0x130308, 0x13A0D4–0x13A170, Caves 0x399F00/0x399FA0 | widecopy, narrowcopy, conv_success, mblen |
| **RomFS-Lookup** findet Dateien nicht (Recompiler-Semantik? Hash `ror`/`udiv`) | 0x212F3C, 0x399244, Caves 0x399F40/0x399FE8 | namelen_exact, chain_end, empty_component |
| **Archiv-Loader / Datei-Größe** | 0x1300C8, 0x1301C0, 0x13C46C, 0x13C4EC, 0x28F33C | bufreturn, real_loader, size_stub (feste BCSAR-Größe!) |
| **cfg:u Sprache/Region** | 0x3CBEFC (`jp`→`uk`) | lang_uk |
| **Thumb-Interworking** | 0x2927B8 | interwork_2927b8 |
| Frame-Callback-Registrierung | 0x11BA98 | frame_callback |
| Menü-Abkürzung (vermutlich überflüssig) | 0x10F8C4 | build_port |

Wichtig: Das Spiel läuft auf echter Hardware mit denselben RomFS-Daten. Dass
der Lookup scheitert, deutet stark auf einen **Semantikfehler im Recompiler**
hin. Zusätzlich verändern `tools/fix_romfs_cycles.py` und
`tools/rebuild_romfs_hash.py` das RomFS-Image, das der Port ausliefert. Auch
das ist ein Workaround.

## 2. Block-Hooks im Recompiler – `tools/workarounds/block_hooks.json` (7)

| Adresse | Wirkung | Ursache (Verdacht) |
|---|---|---|
| 0x1079D0, 0x107A30 | IPC-Reply-Größe 0x28 / r6=0 | `svcSendSyncRequest`-Semantik |
| 0x12E1E4, 0x12E1F0 | Semaphore 0 → 1 (`arm_mutex_bootstrap`) | Kernel-Sync-Objekte |
| nach `bl 0x287DD4` | Allocator 0 → Host-Pool (`arm_pool_alloc`) | Heap-Init (ControlMemory/ExHeap) |
| 0x11A818 | Busy-Wait-Delay sofort beenden | Zeitbasis/Scheduler |
| 0x399244 | uninitialisierten Stack-Slot nullen | Recompiler/ABI |

## 3. Textersetzungen im generierten C – `tools/workarounds/gen_patches.py` (5)

| Funktion | Wirkung | Ursache (Verdacht) |
|---|---|---|
| fn_0028EE00 | srv-Namenslänge 8 → 9 akzeptieren | Spiel schickt `strlen+1`: Recompiler (strlen) oder srv-Layout |
| fn_001187F8 | Fehlerabzweig entfernt + Rückgabe 0 erzwungen | Datei-Open (`rom:`) |
| fn_00100F98 | Asset-Registrierung erzwingen | Save-Flag `[sb+0x567]` |
| fn_001ED64C | fehlende Funktion als Wrapper | Recompiler-Funktionserkennung (Seed fehlt) |
| fn_001B9938 | GPU-Busy-Wait überspringen | GSP-Interrupt P3D fehlt |
| (entfernt) | `.fa`-Read-Hook `hle_fa_read` – war im alten Build nie aktiv | – |

## 4. Runtime – `rg "WORKAROUND(" runtime`

| Kennung | Ort | Wirkung |
|---|---|---|
| `wa-kernel-signal-all` | `runtime/kernel/hle.c` (vblank_thread) | weckt alle Handles + Arbiter pauschal alle 16 ms, ohne Gast-Lock |
| – | `hle_svc` WaitSynchronization | nach 0x400 Yields gelten Handles als signalisiert; Handle 0 gilt als signalisiert |
| – | `hle_svc` ArbitrateAddress | negativer Wert/Timeout setzt das Wort auf 1 |
| – | `dispatch_ipc` | leitet Kommandos nach Form statt nach Handle um (APT/cfg/fs) |
| – | generierter Code | unbekannte Aufrufziele = No-op (geloggt in `out/missing_calls.txt`) |
| – | `arm_mem_read/write` | Zugriffe außerhalb des Speichers liefern 0 statt Abbruch (`ARM_STRICT=1` bricht ab) |

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

**Offen:** Der Fehlerpfad `fn_001028A0 -> fn_0028E410 -> fn_00119D34`
(`g_os+9 = 1`) wird weiterhin ausgeloest. Nach dem Fix laeuft er sauber
ueber den echten Fehlerdienst: `ConnectToPort("err:f")`, dann
`cmd 0x0001 normal=32` mit `p0=C1AC0000 p1=E0A046DB p2=0028EDE8
p3=FFFF8001`; `HLE_ERRCTX` nennt `site=0028EDE8`. Der Hauptthread bleibt
danach vor `ptm:u` stehen (Frontier 4/10). Ursache ist der zu frueh
laufende Worker-Task (Task-Dispatcher `fn_00119D48` setzt ueber `0x27CB2C`
einen Fehlerzustand, den `fn_0028E410` als fatal liest): vermutlich weckt
der HLE-Kernel (`WaitSynchronization`/Arbiter, `wa-kernel-signal-all`) den
Worker zu frueh. Das ist Kernel-Arbeitsvorrat.
