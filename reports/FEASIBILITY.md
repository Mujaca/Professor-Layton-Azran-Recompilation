# Machbarkeitsanalyse — Professor Layton and the Azran Legacy → nativer PC-Port

**Stand:** 2026-11-16
**ROM:** `Professor Layton and the Azran Legacy (Europe).3ds` (1.073.741.824 B)
**Arbeitsverzeichnis:** `C:\Users\user\Documents\Repos\3DS`

---

## 1. Fazit

**Machbar — und deutlich einfacher als bei den meisten 3DS-Titeln.**

| Kriterium | Befund |
|---|---|
| Spielcode-Größe | **2,92 MiB / 755.611 ARM-Instruktionen** (≈ 7.000–8.000 Funktionen) |
| Asset-Dateien | nur **610 Dateien** in 8 Verzeichnissen |
| 3D-Anteil | gering (2D-Visual-Novel-Engine, 3D nur punktuell) |
| Online-Zwang im Gameplay | keiner (nur DLC/StreetPass) |
| Timing-Kritikalität | niedrig (Rätselspiel, keine Physik) |
| Kopierschutz-Sonderfälle | keine (Standard-NCCH, keine Seed-Verschlüsselung, kein Fixed-Key) |
| Eingabe | praktisch nur Touchscreen → Mausabbildung ist trivial |
| Sprachumfang | dieser Dump ist **englisch-only** (`uk`), also nur ein Text-/Voice-Satz |

Damit bleibt als echte Arbeit: **Static Recompilation des Codes + Services/GPU-Shims + Testen**. Die Asset-Pipeline ist risikoarm, weil das Spiel seine Assets selbst liest (die `ARC0`-Archive müssen *nicht* entpackt werden).

---

## 2. Verifizierte Container-Fakten

```
NCSD/CCI, 1 GiB, NCCH @0x4000
  Produktcode       CTR-P-AL6P        (Europa)
  Title-/Program-ID 00040000000F3000
  Hersteller/Version 0x3130 / 2
  NCCH-Flags        no_crypto = true  -> komplette NCCH ist KLARTEXT
  Partitionen       App 909,3 MiB | Manual 4,0 MiB | System-Update 29,3 MiB
```

Wichtig: Die Datei ist **bereits entschlüsselt** (`no_crypto`), es werden **keine Keys/boot9.bin** benötigt.

### ExeFS

| Datei | Größe | Besonderheit |
|---|---|---|
| `.code` | 1.791.432 B | **komprimiert** (Code-LZSS, GodMode9-Variante) |
| `.code` entpackt | **3.059.712 B** | text 0x29A000 + rodata 0x37000 + data 0x1A000 |
| `banner` | 249.112 B | CBMD/CGFX |
| `icon` | 14.016 B | SMDH |
| `logo` | 8.192 B | Nintendo-Logo |

CodeSetInfo aus dem ExHeader:

| Segment | Adresse | Größe |
|---|---|---|
| text | `0x00100000` | `0x29A000` (2,66 MiB) |
| rodata | `0x0039A000` | `0x37000` (0,21 MiB) |
| data | `0x003D1000` | `0x1A000` (0,10 MiB) |
| bss | `0x003EB000` | `0x198A0` |

Einstiegspunkt-Tabelle am Textanfang (`bl`-Sprungtabelle), Prozessname **`layton6`**.

### Build-Fingerprint (aus der Plain-Region)

```
[SDK+NINTENDO:CTR_SDK-4_2_4_200_none]      <- CTR SDK 4.2.4 (2013)
[SDK+NINTENDO:Firmware-02_35_00]
[SDK+NINTENDO:RO] / [SDK+NINTENDO:3DVolume]
[SDK+Mobiclip:Deblocker_1_4_2]
[SDK+Mobiclip:MobiclipDec_1_4_2]           <- Software-Videodecoder!
[SDK+Mobiclip:MoflexDemuxer_1_4_2]
[SDK+Mobiclip:FastAudioDec_1_4_2]
[SDK+Mobiclip:ImaAdpcmDec_1_4_2]
[SDK+Panasonic:HWR_1_0_13]                 <- DSP-Modul
```
`lt6/ctr/version.cfg`: `0.23 $Revision: 814 $ $Date: 2013-09-02`

**Folge:** Der Mobiclip-Videodecoder ist Teil des Spiels (Software-Decode) → beim Recompilieren läuft die Videowiedergabe „gratis" mit, statt einen externen Codec zu brauchen. Das ist ein großer Pluspunkt.

---

## 3. Code-Analyse (dekomprimierte `code.bin`)

```
Instruktionen (ARM-Sweep Revival, 98,8 % Byte-Abdeckung) 755.611
distinkte BL-Ziele (Funktionskandidaten)                  6.573
Prologe `push {…,lr}`                                     7.810
BX/BLX (Interworking)                                     8.616
VFP: vldr 9.258 / vstr 9.198 / vpush 1.389
PC-relative LDR/ADD/SUB                                  17.055
```

- Überwiegend **ARM-Modus** (kein durchgehendes Thumb), also einfacher zu recompilieren.
- **VFPv2 wird benutzt** → Softfloat/FPU-Emulation im Recompiler ist Pflicht (kein NEON).
- 451 SVC-Aufrufe: `svc 0x32` 252× (= `SendSyncRequest`, IPC), `svc 0x23` 89×, `svc 0x28` 18× → klassisches IPC-/Thread-Bild.
- Strings im Code bestätigen die Assetpfade, z. B. `arc/lt6_a.fa`, `arc/lt6_b.fa`, `arc/lt6_@.fa`, `arc/lt6_try.fa`, `ctr/@/icon01.ctpk`, `snd/@/product.bcsar`, `.moflex`, `seq/prog_saveload`.
  Das `@` ist der **Sprach-Platzhalter** → andere Sprachen wären `lt6_de.fa` usw.

### Service-Access-Liste (die HLE-Arbeitsliste)

```
APT:U   fs:USER  gsp::Gpu  hid:USER  dsp::DSP  cfg:u   ptm:u    mic:u
cam:u   ir:USER  y2r:u     soc:U     ssl:C     http:C   boss:U   cecd:u
news:u  ndm:u    nwm::UDS  dlp:FKCL  dlp:SRVR  ac:u     frd:u    ldr:ro
pxi:dev $hioFIO  $hostio0  $hostio1
```

Für einen spielbaren Port kritisch: `fs`, `gsp::Gpu`, `hid`, `apt`, `cfg`, `ptm`, `dsp`/`ndsp`.
Nur optional: `soc/ssl/http` (DLC/Update), `boss`, `cecd`, `news`, `nwm` (StreetPass/Online), `cam`, `mic`, `ir`, `y2r`.

---

## 4. Asset-Inventar (RomFS: 610 Dateien, 900,1 MiB)

| Typ | Anzahl | Größe | Format / Befund |
|---|---|---|---|
| `.fa` | 3 | **425,3 MiB** | Level-5 **`ARC0`**-Archiv, Einträge komprimiert/ohne Namen; `lt6_a.fa` (5 MB), `lt6_b.fa` (277 MB), `lt6_uk.fa` (163 MB) |
| `.bcstm` | 566 | **265,3 MiB** | NintendoWare-Streams, **DSP-ADPCM** (Musik, `BGM_*.dspadpcm.bcstm`) |
| `.moflex` | 34 | **190,0 MiB** | **Mobiclip-Video** (Magic `4C 32 AA AB`), Anime-Cutscenes |
| `.bcsar` | 1 | 15,2 MiB | `CSAR` – Soundarchiv (SFX/Stimmen) |
| `.cfa` | 1 | 4,0 MiB | elektronisches Manual (enthält eigene NCCH) |
| `.ctpk` | 2 | 4,7 KiB | Textur-/Icon-Pakete |
| Rest | 4 | – | `banner.bnr`, `banner.icn` Entity, `version.cfg` |

Struktur: `lt6/arc/*`, `lt6/ctr/uk/*`, `lt6/mov/*.moflex`, `lt6/snd/uk/{product.bcsar,stream/*}`.

**Wichtig:** `.fa` muss für einen Port **nicht** entpackt werden — das Spiel liest seine Archive selbst (Code wird ja mitrecompiliert). Nur wenn wir Videocodec/Audio ersetzen wollen, lohnt Extraktion; BCSTM/MOFLEX sind dokumentiert bzw. haben Community-Decoder.

---

## 5. Was noch offen ist (nur per Laufzeit-Trace zu klären)

Statisch *nicht* ermittelbar, weil die `.fa`-Daten komprimiert sind:

| Frage | Warum wichtig | Wie klären |
|---|---|---|
| Wie viele Shader-/TEV-Konfigurationen? | bestimmt Umfang des GPU-Shims | Trace der GPUREG-Writes + Shader-Binaries (`DVLP`/`DVLE`) |
| Wird überhaupt ein Vertex-Shader benutzt? | 2D-Quads brauchen ggf. keinen | GPUREG-Trace |
| Welche GPU-Kommandos/Blendmodi? | Renderer-Vollständigkeit | Command-Stream-Trace |
| Thread-Struktur / Frame-Pacing | Determinismus | SVC-/Thread-Trace |
| Wird `mic:`/`cam:`/`ir:` im Gameplay gebraucht? | Fehlerquelle bei Stubs | IPC-Trace pro Kapitel |
| Nutzt das Spiel DLC (`MountAddOnContent`)? | tägliche Rätsel fehlen (kein `.cia` vorhanden) | `fs`-Trace |

---

## 6. Risiken

| Risiko | Bewertung | Gegenmittel |
|---|---|---|
| ARM/Thumb-Funktionsgrenzen | **mittel** – 8.616 BX/BLX, aber dominant ARM | dynamischer Block-Trace aus Azahar statt Ghidra-Raten |
| VFP-Rundung | niedrig | Differenztests pro Funktion |
| `ARC0`-Format | **kein Risiko** (Spiel liest selbst) | – |
| Mobiclip-Video | **kein Risiko** (Software-Decoder im Code) | nur Perf: ggf. Frames pre-dekodieren |
| DSP/ADPCM-Audio | niedrig–mittel | BCSTM → Host-Audiopfad oder DSP-Interpreter aus Azahar |
| 2D-Renderer | **niedrig** – texturierte Quads, wenig TEV | Batch-Renderer (Vulkan/SDL3) in Tagen |
| Content-Testing (viele Rätsel) | mittel (Fleißarbeit) | Input-Replays + Frame-Hashes |
| DLC (365 Tagesrätsel) | Scope-Frage | DLC-`\.cia\ (0004008C000F3000) optional mounten |

---

## 7. Empfohlene Reihenfolge

1. **Trace-Harness** (Azahar patchen): Block-Ausführung, SVC, IPC-Requests, `fs`-Öffnungen, GPUREG-Writes, Buffer-Swaps → JSONL. Einmal durchspielen.
2. **Recompiler** (ARMv6K + VFPv2, C-Codegen) mit Differenztests gegen Emulator.
3. **Runtime-Skeleton**: Memory-Map, SVC-Shims, `srv`/`apt`/`fs` → Boot bis Menü.
4. **GPU-Shim** für die im Trace gefundenen Konfigurationen (2D-first).
5. **Audio**: BCSTM-Streams → Host (oder DSP-Interpreter), `hid` → Maus/Gamepad.
6. **Vollständigkeit**: Saves (`fs`-Shim), Sprache, DLC, Feinschliff (HiDPI, Widescreen).

Aufwandsschätzung (1 Entwickler, Feierabend): Menü lauffähig ≈ 1 Monat, komplett durchspielbar ≈ 5–7 Monate.

---

## 8. Reproduzierbar: Tooling in diesem Repo

| Skript | Zweck |
|---|---|
| `tools/nokeys.py` | pyctr ohne boot9/Keys (nur für entschlüsselte Images) |
| `tools/explore.py` | NCSD/NCCH-Struktur, Magics-Scan |
| `tools/extract.py` | ExeFS extrahieren, RomFS-Inventar → `reports/romfs_listing.tsv` |
| `tools/decomp.py` | `.code` entpacken → `out/exefs/code_decomp.bin` |
| `tools/codestats.py` | Mnemonics-, SVC-, ARM-Sweep-Statistik |
| `tools/code_strings.py` | Strings, BL-Ziele, VFP-/Prolog-Zählung |
| `tools/assetfmt.py` | Header der Asset-Typen (ARC0/CSAR/CSTM/MOFLEX) |
| `tools/fa_scan.py` | `ARC0`-Header-Felder, Entropie, Namenssuche |
| `tools/exheader.py` | ExHeader/CodeSetInfo/Service-Liste |

Reproduktion:

```powershell
python tools\extract.py
python tools\decomp.py
python tools\codestats.py
python tools\code_strings.py
```
