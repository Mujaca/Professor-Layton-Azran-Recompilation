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

- Nur EIN `[key]`-Lookup; danach bleibt der Hauptthread in der Dir-Hash-Kette
  (`0x212D70`/`0x212DA0`) stehen. Weitere Dateien (Fonts/Assets, `head.lt6`)
  werden nicht geladen.
- `boss:U`, `/head.lt6`, `SetBufferSwap (0x0005)` und ein echter Frame fehlen.
