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

## Offen

- Die Init-Schleife `0x101508`/`0x10151C` wartet auf `*(u16*)(0x3EC028+0x2144)`
  (= `0x3EE16C`). Memwatch zeigt: **nur** BSS-Clear und ein Write von `0`
  (`pc=0x288398`, `lr=0x2BA728`) — der Loader setzt das Flag nie auf 1.
  In 90 s läuft die Schleife ~6400×, `0x101540` wird nie erreicht.
- Ein `STR [Rn,#0x2144]` existiert im Image nicht (auch kein `ADD #0x2144`);
  der Setter muss den Offset anders bilden oder das Flag per Kopie/DMA schreiben.
- `.fa`-Dateien werden **nicht über FS** geöffnet: `HLE_FS_PATHS` zeigt nur
  SelfNCCH, `OpenArchive id=7`, `/gamecoin.dat` und drei `file 0x0802`-Reads.
  Die `.fa`-Geräte bekommen also entweder leere Synthese-Dateien oder werden
  gar nicht geöffnet.
- Die `0x0802`-Reads tragen **Binärpfade** (Azahar: `NCCHFilePath`, 0x14 Byte:
  `{open_type, content_index, filepath_type, exefs_filepath[8]}`) — unser
  `find_path` wertet sie nicht aus und liefert damit falsche/leere Daten.
- Mit `HLE_SCENE_READY=1` erreicht die Engine die RM-Frame-Schleife
  (`0x288AF8/0x288B34`) und sendet `gsp 0x0006/0x000C` samt `[gpusubmit]`.
- `boss:U`, `/head.lt6`, `SetBufferSwap (0x0005)` und ein echter Frame fehlen.
