# Layton 6 (Azran Legacy) – statische Recompilation für den PC

Ziel: *Professor Layton and the Azran Legacy* (3DS, EUR) als **native
PC-Anwendung** – ohne Emulator. Der ARM11-Code wird statisch nach C übersetzt
(`tools/recomp`), eine eigene Runtime (`runtime/`) stellt Kernel, Dienste,
GPU und Fenster bereit. Azahar dient nur als Referenz zum Lesen und Vergleichen.

**Für Agenten: zuerst [`AGENTS.md`](AGENTS.md) lesen.**

| Dokument | Inhalt |
|---|---|
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Ziel, Meilensteine, Frontier-Verlauf |
| [`docs/WORKAROUNDS.md`](docs/WORKAROUNDS.md) | technische Schuld (soll schrumpfen) |
| [`docs/REFERENCE.md`](docs/REFERENCE.md) | verifizierte 3DS-Fakten |
| [`docs/TOOLS.md`](docs/TOOLS.md) | Werkzeuge |
| [`reports/FEASIBILITY.md`](reports/FEASIBILITY.md), [`reports/RECOMPILER_MVP.md`](reports/RECOMPILER_MVP.md), [`reports/TRACE.md`](reports/TRACE.md) | Hintergrundberichte |

## Stand

Natürlicher Boot ab `0x00100000` erreicht 7 von 10 Referenzdiensten
(APT, ndm, fs, hid, ptm, cfg, gsp); erste Divergenz: `y2r:u`. Noch kein vom
Spiel gerenderter Frame. Details und Verlauf: `docs/ROADMAP.md`.

## Quickstart

```powershell
pip install pyctr capstone keystone-engine ziglang
git submodule update --init tools/azahar     # optional, nur als Referenz

python tools/extract.py          # ExeFS + RomFS-Listing (ROM im Repo-Wurzelverzeichnis)
python tools/extract_romfs.py    # RomFS-Dateien -> out/romfs
python tools/romfs_image.py      # SelfNCCH-Image -> out/romfs_image.bin
python tools/build.py            # -> out/port.exe (erster Lauf: einige Minuten)
python tools/frontier.py         # Fortschritt messen
.\out\port.exe                   # Fenster: oben 400x240, unten 320x240
```

## Struktur

```
tools/recomp/      ARM11 -> C Recompiler (spielunabhängig)
tools/workarounds/ alle spielspezifischen Abweichungen (Schuldenliste)
tools/g3ds/        Analysewerkzeuge (trace-diff, guest-refs, ...)
runtime/cpu/       CPU-Zustand, Instruktionssemantik, Block-Hook/Trace
runtime/kernel/    SVCs, Threads, IPC, 3DS-Dienste
runtime/gpu/       PICA200-Ausführung, Texturdekoder
runtime/platform/  Win32-Fenster
runtime/main.c     Host: lädt Code, startet 0x00100000, zeigt Framebuffer
tests/             Recompiler-/Trace-/GPU-Selbsttests
reports/           Referenzspuren und -bilder (Azahar), Berichte
```

## Rechtliches

Die ROM wird nicht mitgeliefert. Der Port bezieht seine Daten aus der eigenen
Cartridge-Kopie des Nutzers.
