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
| `wa-fs-client-slot` | `runtime/kernel/hle.c` (srv/fs) | schreibt fs-Handles direkt an 0x3D91D0 / 0x3D66C4 |
| `wa-kernel-signal-all` | `runtime/kernel/hle.c` (vblank_thread) | weckt alle Handles + Arbiter pauschal alle 16 ms, ohne Gast-Lock |
| – | `hle_svc` WaitSynchronization | nach 0x400 Yields gelten Handles als signalisiert; Handle 0 gilt als signalisiert |
| – | `hle_svc` ArbitrateAddress | negativer Wert/Timeout setzt das Wort auf 1 |
| – | `dispatch_ipc` | leitet Kommandos nach Form statt nach Handle um (APT/cfg/fs) |
| – | generierter Code | unbekannte Aufrufziele = No-op (geloggt in `out/missing_calls.txt`) |
| – | `arm_mem_read/write` | Zugriffe außerhalb des Speichers liefern 0 statt Abbruch (`ARM_STRICT=1` bricht ab) |

## 5. Bekannter Verdacht (noch nicht geprüft)

Laut Code-Review beginnen viele IPC-Antworten in `hle.c` mit `ipc_put(r, 0)`
als „Result“ statt mit dem Platzhalterwort für den Header. Dann sind alle
weiteren Wörter um eins verschoben (z. B. hid GetIPCHandles, APT
GetLockHandle, fs OpenArchive, gsp RegisterInterruptRelayQueue). Das würde
erklären, warum Setter im Spiel „nie laufen“ (`wa-fs-client-slot`). **Erster
Prüfpunkt**, weil hid:USER direkt vor dem Hänger liegt.
