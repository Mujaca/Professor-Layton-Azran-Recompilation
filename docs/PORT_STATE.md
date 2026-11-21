# Aktueller Laufzeit-Zustand (Archiv-Mount + Datei-Loader)

## Patches in `out/exefs/code_decomp.bin` (Skripte in `tools/`)

| Skript | Wirkung |
|---|---|
| `patch_mount_skip.py` | optional: überspringt `fn_0011E494` (Diagnose) |
| `patch_interwork_2927b8.py` | Thumb-Stub für den ARM-Veneer `0x2927B8` |
| `patch_empty_component.py` | überspringt die leere erste Pfadkomponente (Code-Cave 0x399FE8) |
| `patch_lang_uk.py` | Region-Literal `jp` → `uk` |
| `patch_widecopy.py` | `0x13023C` → Cave 0x399FA0 (Narrow→Wide 1:1) |
| `patch_narrowcopy.py` | `0x130270` → Cave 0x399F00 (Wide→Narrow 1:1) |
| `patch_mblen.py` | mblen(−1) als 1 Byte (inzwischen durch widecopy umgangen) |
| `patch_conv_success.py` | `0x13028C` → `b 0x1302A8`, `0x130308` → `mov r0,#1` |
| `patch_size_stub.py` | `0x13C46C` liefert 0x00F3DE78 (BCSAR-Größe) |
| `patch_bufreturn.py` | `0x1300C8` = `mov r0,r1; bx lr`; `0x1249CC` = `b 0x124C30` |

## Runtime-HLE (Umgebungsvariablen)

| Variable | Wirkung |
|---|---|
| `HLE_FILE_PRELOAD=1` | lädt den Datei-Inhalt direkt aus `out/romfs_image.bin` in den Zielpuffer (`size`→`dataOffset` aus der File-Meta-Tabelle) |
| `HLE_BIGARENA=1` | lenkt zu große Arena-Allokationen auf `base=0x18000000 / cap=0x04000000` um |
| `HLE_QUEUE_LOG=1` | `[branch]`, `[lf]`, `[key]`, `[a08]`, `[obj]` |
| `HLE_STREAM_LOG=1` | `[stream]`-Reads von `0x205858` |
| `HLE_ALLOC_LOG=1` | Arena-Allokationen |
| `HLE_ROMFS_NO_STRIP=1` | IVFC-Präfix NICHT abziehen (Standard: abziehen) |

## Erreicht (belegt)

- RomFS-Metadaten korrekt geparst (IVFC-Strip), Lookup trifft (`0x212FA4`, `0x2808F4`).
- Dateigröße/Alloc: 0x00F3DE78, Puffer 0x18000000.
- Datei-Inhalt vorgeladen: `[preload] size=00F3DE78 dataOff=26B93A40 dst=18000000 got=15982200`.
- Mount-Fehlerpfad verlassen: `[romfs] 002850B8` **und** `[romfs] 00285184` (Rückkehr über den Erfolgszweig).
- GPU: `gsp::Gpu cmd=0x0006` und `0x000C`; `[gpusubmit] handle=42 list=00410000 size=28`.

## Menü-Assets und Szenen-Strings (neu)

Im Code liegt ab `0x28FFDC` eine ASCII-Namensliste der Menü-Ressourcen:

```
0028FFDC  menu_logo
0028FFF4  menu_title
00290000  menu_name
00290018  menu_finish
00290024  menu_bag
00290030  menu_invest
0029003C  menu_fukamaru
0029004C  menu_jiten
```

Wichtig: **kein** `ldr [pc]`-Literal zeigt auf diese Adressen (geprüft über
alle LDR-Immediates und alle 32-Bit-Wörter im Image). Die Namen werden also
indirekt (Manifest-/Offset-Tabelle) ausgewertet, nicht als C-Strings benutzt.

Szenen-Verwaltung: `0x3EC028` ist das globale Szenen-Objekt. Zugriffe:

| Adresse | Bedeutung |
|---|---|
| `0x10F188` | `base+0x2000+0x104`, `ldrh [.+0x40]` → **`0x3EE16C`** (Warteflag) |
| `0x287694` | `base+0x2104` (Szenen-Unterobjekt) |
| `0x2876A0` | liest `[.+0x40]` und `[.+0x38]` (Getter) |
| `0x28F7BC` | Szenen-Update (virtueller Call über `[obj]→[+0x40]`) |
| `fn_00115C18` | `base+0x2000`, `+0x144`, `0x28EED0` (0x200 Byte nullen = Reset) |

Da das Flag das **Längenfeld eines Wide-Strings** ist, schreibt es ein
String-Assign (Bibliotheksfunktion), kein `STRH`. Genau deshalb half
`HLE_SCENE_NAME` (setzt Länge+Zeichen) und `HLE_SCENE_READY`.

## Menü-Szene: Zustand und Asset-Requests (neu)

`fn_0028F7BC` ist das **Szenen-Update** (wird auch von der Init-Schleife
über `0x10153C` aufgerufen). Es verteilt über `r4` (Zustands-ID) mit den
Konstanten `0xCEE`, `0xBB9`, `0x834`, `0x69A` (Literal-Pool `0x28FF98…`).

**Menü-Zustand = `r4 = 0x7D0`:**

```
0x28F894  cmp r4, 0x834          ; r4 < 0x834
0x28F8A8  r1 = 0x834 - 0x320 = 0x514
0x28F8B8  bgt 0x28F8E0           ; r4 > 0x514
0x28F8E0  cmp r0, #0x2BC         ; r0 = r4 - 0x514
0x28F8E4  beq 0x28FCE8           ; menu_logo
```

Daraus folgt `r4 = 0x514 + 0x2BC = 0x7D0`.

Der `menu_logo`-Block (`0x28FCE8`) lädt den Namen aus der Tabelle
(`0x28FFDC`, ASCII) und baut daraus eine getaggte Variante:

```
0x28FCE8  add r1, pc, #0x2EC     ; -> "menu_logo"
0x28FCEC  r0 = sl + 0x30
0x28FCF0  bl 0x28A164            ; Variante: Typ 8, [+4]=Namezeiger
0x28FD20  bl 0x28A1BC            ; Ressourcen-Request einreihen (obj+0x914)
```

**Rezept zum Anstoßen (ohne Spiel-Eingaben):**

```
HLE_FRAME_LOOP=28F7BC
HLE_FRAME_ARG=3EC028,7D0,0,FFFFFFFF
HLE_MENU_LOG=1        # loggt 0x28F7BC / 0x28FCE8 / 0x28A164 (Name!) / 0x28A1BC
HLE_MENU_FORCE=1      # setzt r0 an 0x28F8E0 auf 0x2BC
```

Beleg (90-s-Lauf):

```
[menu] pc=0028FCE8 r0=000002BC r1=00000514
[menu] pc=0028A164 r0=0FFEFF80 r1=0028FFDC name='menu_logo'
[menu] pc=0028A1BC r0=003EC028 r1=0FFEFFD0 r2=000007D0
```

Damit läuft der **echte Menü-Szenencode** und stellt `menu_logo` an.
Es entstehen aber noch **keine Pixel**: `out/shot_menu.bmp` ist
byte-identisch zum Platzhalter (`0` Byte Unterschied, beide 288.054 B),
und `gsp::Gpu` sendet weiterhin nur `0x0006`/`0x000C` (kein `0x0005`).

## Menü-Draw-Pfad (neu)

Die Request-Kette des Menüs ist vollständig belegt:

```
0x28FCE8  menu_logo-Zweig
0x28A164  getaggte Variante {Typ 8, [+4]=Namezeiger}
0x28A1BC  Request einreihen  -> r0 = obj+0x2104 (= 0x003EE12C)
0x1117A4  Consumer           -> 0x2800C8
0x2800C8  Draw-Objekt bauen (r2=2, r3=Name-Variante)
```

Messung (60-s-Lauf mit `HLE_FRAME_LOOP=28F7BC`,
`HLE_FRAME_ARG=3EC028,7D0,0,FFFFFFFF`):

```
[menu] pc=0028A1BC r0=003EC028 r1=0FFEFFD0 r2=000007D0
[menu] pc=001117A4 r0=003EE12C r1=0FFEFF18 r2=0FFEFFD0 r3=000007D0
[menu] pc=002800C8 r0=003EE12C r1=0FFEFEFC r2=00000002 r3=0FFEFFD0
```

-> **441 Draw-Objekte** fuer `menu_logo` werden gebaut. Es entstehen aber
weiterhin keine Pixel (Screenshot byte-identisch), weil dem Draw-Objekt die
geladenen Asset-Daten (Dimensionen/Textur) fehlen.

Zusatzmessung: der Menue-Zustand loest noch **keine** Archiv-Lookups aus —
`[key]` = 0, `[path]` nur `$coin:/gamecoin.dat`, `[file] Read` = 7
(nur Metadaten). Die Namensaufloesung fuer `menu_logo` passiert also erst
im Ressourcen-Manager, der ohne geladene `.fa`-Tabelle nicht arbeitet.

### Frueher Abbruch im Draw-Builder (neu)

`0x2800C8` bricht sofort ab, wenn `[drawlist+0x6a] == 0`:

```
0x2800F0  ldrh r1, [r5, #0x6a]      ; r5 = 0x003EE12C
0x280100  add  r6, r5, #0x44
0x280108  movls r4, fp (=0)
0x28010C  bls  0x2801CC
0x2801CC  cmp  r4, #0
0x2801D0  beq  0x280338            ; Abbruch
```

Mit `HLE_DRAW_FIX=1` (setzt `[obj+0x6a]=1`) laeuft der Builder weiter und
der Map-Lookup wird erreicht:

```
[res] pc=00288960 r0=0FFEF714 r1=BF800000 r2=000000AC r3=00000002 map0=003EC93D
[res] pc=0028031C r0=003EE198
```

`r1 = 0xBF800000` ist `-1.0f` (also ein Transform-/Material-Schluessel, kein
Namens-Hash). Danach endet der Builder regulaer.

**Aber:** `out/shot_drawfix.bmp` ist weiterhin byte-identisch zum Platzhalter
(`0` Unterschiede von 288.054 Byte), und `gsp::Gpu` sendet kein `0x0003`/`0x0005`.
Das heisst: die Draw-Objekte werden gebaut, aber **nicht submitted**.

### GSP-Kommandos und ihre Absendestellen (neu)

Der generische IPC-Logger (`[ipc] … blk=<guest-block>`) liefert die
Aufrufstellen der GSP-Kommandos:

```
blk=0012F954  gsp::Gpu cmd=0x0016 n=1 t=2 p0=00000000
blk=001427EC  gsp::Gpu cmd=0x0013 n=1 t=2 p0=00000001
blk=0027604C  gsp::Gpu cmd=0x0010 n=1 t=0 p0=00000203
blk=0027604C  gsp::Gpu cmd=0x001E n=2 t=0 p0=00000010 p1=00000011
```

Das sind PICA-Register-Schreibzugriffe (Init), **kein** `0x000C`
(TriggerCmdReqQueue) und **kein** `0x0005` (SetBufferSwap). Unser HLE
fuehrt bei `0x000C` die Kommandoliste aus (`gpu_run_command_list`), es wird
aber nie ausgeloest.

Wichtig (Azahar-Referenz, `gsp_gpu.cpp:833`): `TriggerCmdReqQueue` arbeitet
dort **nicht** mit `SetCommandList`, sondern mit dem Kommandopuffer im
GSP-Shared-Memory (`GetCommandBuffer(thread_id)`), also einer Ringliste mit
`SubmitCmdList`-Eintraegen. Unser HLE-Pfad (`g_cmdlist_addr` aus `0x0006`)
ist daher zu einfach: das Spiel sendet `0x0006` gar nicht.

**Implementiert (`runtime/hle/hle.c`):** `gsp_process_cmdqueue()` liest das
Azahar-Layout

```
base  = GSP-Shmem + 0x800 + thread_id*0x200
+0x00 Header {index:8, number_commands:8, status:8, should_stop:8}
+0x20 Command[15] je 0x20 B; id 0x01 = SubmitCmdList
       +0x04 Listenadresse, +0x08 Laenge
```

und fuehrt die Listen ueber `gpu_run_command_list` aus (Log `[gspq]`).
Aufgerufen bei gsp-Cmd `0x000C` und optional im Frame-Pump (`HLE_GSPQ_PUMP=1`).

**Messung:** Im Port ist der Bereich `0x10002800..` komplett null
(`HLE_DUMP_RANGE=10002800,10003200` → alle Werte `00000000`), und es erscheint
kein `[gspq]`. Das Spiel fuellt den Kommandopuffer also gar nicht — passend
dazu, dass `0x000C` nie gesendet wird. Die eigentliche Ursache liegt damit
weiterhin im Grafik-Flush des Spiels (Render-Thread/Frame-Pfad).

Die Zuordnung ist verifiziert (`HLE_MAP_LOG`):

```
[map] svc=1F r0=0000001C r1=10000000 ...   (HID-Shmem)
[map] svc=1F r0=00000000 r1=10002000 ...   (GSP-Shmem)
```

Der GSP-Shared-Memory liegt also wirklich bei `0x10002000`; der Puffer ab
`+0x800` ist korrekt adressiert, wird aber von keinem Spiel-Thread beschrieben.

Memwatch-Beleg (`ARM_MEMWATCHRANGE=10002800,0x200`, 45 s): genau **ein**
Schreibzugriff, naemlich das Nullsetzen des Headers:

```
[mw] 10002800 <- 00000000 size=4 pc=0014250C lr=0013EBC8 th=2
```

Danach wird nie ein Kommando eingetragen. Mit `HLE_GSP_IRQ=1` +
`HLE_GSPQ_PUMP=1` + Menue-Zustand bleiben die GSP-Kommandos unveraendert
(`0x0016/0x0013/0x0010/0x001E`), kein `0x000C`, kein `0x0005`, kein `[gspq]`.
Der Render-/Submit-Thread laeuft also weiterhin nicht durch.

Gast-Threads (`HLE_THR_LOG`): das Spiel erzeugt acht Threads, alle mit
Entry `0x00123168` und eigenen Stacks:

```
handle=7  stack=0040F000  id=1     handle=34 stack=0040DB98 id=3
handle=16 stack=003D1FE0  id=2     handle=35 stack=0040EB98 id=4
handle=36 stack=0900F180  id=5     handle=48 stack=003D8C98 id=6
handle=57 stack=14126040  id=7     handle=58 stack=1412C8E8 id=8
```

Die Threads existieren also; sie warten aber (Watchdog zeigt sie in
Spin-/Wait-Helfern wie `0x287B90`) und stossen den GX-Flush nicht an.

## Echter Loader ohne Short-Circuit (neu)

`python tools/patch_real_loader.py` (nach `decomp.py` + `apply_all_patches.py`)
nimmt den `0x1249CC`-Short-Circuit zurück und setzt die vier Stellen, die der
echte Mount zusätzlich braucht:

```
0x1249CC  add r0,r6,#0x2DC        (Original)
0x28F33C  mov r8,r3               (Copy meldet konsumierte Größe)
0x28F340  b 0x28F39C
0x1301C0  nop                     (consumed-Check)
0x12FD30  mov r0,#1 ; bx lr       (0x139914-Ergebnis als Erfolg)
```

Runtime-Schalter: `HLE_OBJ_FIX=1 HLE_SPAN_FIX=1 HLE_REG_FIX=1`
(plus `HLE_FILE_PRELOAD=1 HLE_BIGARENA=1`).

Was die drei Schalter tun:

| Schalter | Wirkung |
|---|---|
| `HLE_OBJ_FIX` | Scratch-Container `P=0x0CFFE000` (`[P]=1`, `[P+4]=1`, `[P+8]=1`, `[P+0x2C]=[P+0x34]=0`), `[[archive+0x1A4+4]+0x3C]=P`; macht `0x140140`/`0x14015C`/`0x1401B0` zu gültigen Count-/Info-Quellen |
| `HLE_SPAN_FIX` | setzt die Span-Magic `[archive+0x724]=1`, ohne die `0x284A48` immer 0 liefert |
| `HLE_REG_FIX` | stellt an `0x124B6C` `r7`/`sl` bereit (Span-Assign braucht `r1!=0 && r3!=0`) |

Beleg (180-s-Lauf, `0x1249CC` original):

```
[chain] pc=00124AD8 r0=00000001
[chain] pc=00124B6C r0=19E88EC0
[chain] pc=00124C30 r0=00000001     <- fn_0012493C = Erfolg
2850B8: 1   285184: 1   preload: 1
```

Damit ist `0x2850B8=1` **nicht mehr** vom Short-Circuit abhängig.
Der Szene-Flag-Pfad (`0x3EE16C`) bleibt davon unberührt: er ist weiterhin 0,
`boss:U`/`/head.lt6` und `.fa`-Reads folgen nicht.

## Mount-Rückgabekette (neu belegt)

`0x2850A8: bl 0x11E494; movs r8,r0` → `0x2850B8` ist exakt der Rückgabewert
von `fn_0011E494`, und dieses gibt `r5` = Rückgabe von `fn_0012493C` zurück
(`0x11E4EC bl 0x12493C`, `0x11E510 mov r0,r5`).

`fn_0012493C` endet erfolgreich nur, wenn `0x284A48(obj+0x720, …) != 0`
(`0x124C14 cmp r0,#0; 0x124C18 movne r0,#1`). Ist die Registrierung 0,
läuft es über `0x124C28 bl 0x284BE4` und liefert deren Wert (bei uns 0).
Damit ist klar: Mount-Erfolg = erfolgreiche **Registrierung des Datei-
Eintrags**, nicht nur eine gelungene Größenrechnung.

## Reproduzierbarer Patch-Satz
`python tools/decomp.py` (Original aus `code.bin`) und danach

```
python tools/apply_all_patches.py            # inkl. Diagnose-Caves
python tools/apply_all_patches.py --no-retprobe
```

spielt alle produktiven Patches in fester Reihenfolge auf (Caves ueberlappen
sich nicht). Reihenfolge/Adressen siehe `tools/apply_all_patches.py`.

## Offen

- Die Init-Schleife `0x101508`/`0x10151C` wartet auf `*(u16*)(0x3EC028+0x2144)`
  (= `0x3EE16C`). Memwatch zeigt: **nur** BSS-Clear und ein Write von `0`
  (`pc=0x288398`, `lr=0x2BA728`) — der Loader setzt das Flag nie auf 1.
  In 90 s läuft die Schleife ~6400×, `0x101540` wird nie erreicht.
- Ein `STR [Rn,#0x2144]` existiert im Image nicht (auch kein `ADD #0x2144`);
  gefunden wurde nur **ein** Paar `ADD #0x2000` + `ADD #0x144`
  (`0x115D8C`/`0x115D90`, `fn_00115C18`) — dort wird das 0x200-Byte-Objekt
  bei `scene+0x2144` per `0x28EED0` **genullt** (Reset, nicht Setter).
  Zusätzlich 20 Stellen `add #0x2000` + `STR [.,#0x144]` (u. a. `0x21B408`,
  `0x2FE1CC`) — Kandidaten für den Setter, aber Index-/Array-Stores.
- Es gibt **keinen** gespeicherten Zeiger auf `0x3EE16C` im Scene-Bereich
  (Dump 0x3EC028..0x3EE200, kein Wert `003EE16C`) — der Setter muss die
  Adresse selbst bilden.
- `.fa`-Dateien werden **nicht über FS** geöffnet: `HLE_FS_PATHS` zeigt nur
  SelfNCCH, `OpenArchive id=7`, `/gamecoin.dat` und drei `file 0x0802`-Reads.
  Die `.fa`-Geräte bekommen also entweder leere Synthese-Dateien oder werden
  gar nicht geöffnet.
- Die `0x0802`-Reads tragen **Binärpfade** (Azahar: `NCCHFilePath`, 0x14 Byte:
  `{open_type, content_index, filepath_type, exefs_filepath[8]}`) — unser
  `find_path` wertet sie nicht aus und liefert damit falsche/leere Daten.
- Mit `HLE_SCENE_READY=1` erreicht die Engine die RM-Frame-Schleife
  (`0x288AF8/0x288B34`) und sendet `gsp 0x0006/0x000C` samt `[gpusubmit]`.
- Der volle Loaderpfad (ohne Short-Circuit an `0x1249CC`) läuft über den
  zweiten Größen-Accessor `0x13C4EC` (jetzt ebenfalls auf `0x00F3DE78`
  gestubbt) in `fn_0028F324` → `0x118798` und hängt dort im **Virtuellen
  Aufruf** `0x1187E8 blx ip` (Watchdog: `pc=0x28F35C`, `lr=0x1187EC`).
  Ein Patch von `0x27E3C8` (vermuteter Wait) half nicht.
- **Neu (Sonde `[countfix]`/`[p132]`, `HLE_COUNT_FIX`):** Der echte Loader
  scheitert an `fn_00140140`. `0x132FF8(obj+0x334, obj+0x1A4)` ruft
  `0x140140(obj+0x1A4)`; das ist **kein** einfacher Getter, sondern die Kette
  `[obj+4] → [+0x3C] → call → [r0]` (Rohcode ab 0x140140: `ldr r0,[r0,#4]`,
  `nop`, `push {r4,lr}`, `ldr r0,[r0,#0x3C]`, `bl …`, `ldr r0,[r0]`).
  Für unser Archiv-Objekt `0x0D400260` (`[+4]=0x0D400398`, nicht
  initialisiert) liefert sie `0x00FEFF52` → Groesse `0x03FBFD4C` (64 MB)
  → `0x284AA8` scheitert → Fehlerrueckkehr nach `0x124C2C`.
  Mit `HLE_COUNT_FIX=0` (schreibt `[obj+0x1A8]=0`) wird die Groesse 4 und
  `0x12FC24` laeuft echt durch; danach liefert `0x133018` 0 → Alloc(0)
  scheitert. Mit `HLE_COUNT_FIX=1` kommt wieder 0x03FBFD4C, weil die echte
  Kette den Wert ueberschreibt. Zu fixen ist also das **Objekt
  `0x0D400260`** selbst (Feld `+4` = gueltige Listen-/Container-Instanz).
- **Neu (`HLE_OBJ_FIX`):** Mit einer Scratch-Struktur `P = 0x0CFFE000`
  (`[P]=1`, `[P+8]=1`, `[P+0x2C]=[P+0x34]=0`) und
  `[[obj+4]+0x3C] = P` liefern `0x140140` **und** `0x14015C` einen Count von 1.
  Ergebnis (voller Pfad, Copy-Bypass): alle vier Allocs gelingen
  (`0x00F3DE78`, `0x00F3DE78`, `8`, `0x5E0`, `0xA000`) und der Lader erreicht
  erstmals `0x124AD8` (hinter `0x12FD30`) — vorher brach er bei `0x124A28` ab.
  `0x12FD30` selbst liefert 0 (Abbruch ueber `beq 0x124C30`); mit
  `0x12FD30 = mov r0,#1` laeuft der Lader weiter, erreicht `0x124C30` aber
  nicht in 120 s (Hauptthread idle bei `0x283488`, `sp=0x0CF0F000`).
  `HLE_FLAG_FIX` (Byte `0x3E7771`, gelesen von `0x139844`) aenderte daran
  nichts: der Wert ist bereits `1` (Wort `0x3E7770 = 0x00010100`), `0x139844`
  liefert also 1 — der 0-Rueckweg von `0x12FD30` kommt aus einem spaeteren
  Zweig. Auch mit Stub (0x12FD30 = mov r0,#1) bleibt der Lader nach `0x124AD8` in 240 s stecken
  (`0x124C30` wird nicht erreicht, Watchdog `0x283488`).
- Mit dem kurzen Pfad (`0x1249CC → 0x124C30`) bleibt der Mount-Erfolg
  reproduzierbar.
- `boss:U`, `/head.lt6`, `SetBufferSwap (0x0005)` und ein echter Frame fehlen.

### Screenshot-Timing (wichtig!)

`tests/port.c` speicherte `--shot` frueher bei **Frame 3** (~48 ms) und brach
bei Frame 6 ab. Das Spiel hat zu diesem Zeitpunkt noch nichts gerendert —
alle bisherigen "288.054 B, 0 Unterschiede"-Befunde waren dadurch
vorbestimmt. Neu: `PORT_SHOT_FRAMES=<n>` legt den Aufnahmezeitpunkt fest
(Default 3). Beleg mit `HLE_GPU_SUBMIT=1` (synthetischer Frame):

```
PORT_SHOT_FRAMES=200 -> shot_late.bmp: 288054 B, Unterschiede: 284003
```

Damit ist bewiesen: die VRAM-Pixel werden korrekt in den Screenshot
uebernommen, sobald sie existieren. Ohne Fremd-Submit (nur Spiel-Pixel)
bleibt es bei 0 Unterschieden — das Spiel schreibt weiterhin nichts in den
VRAM (siehe GSP-Kommandopuffer).
