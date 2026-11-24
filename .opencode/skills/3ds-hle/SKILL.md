---
name: 3ds-hle
description: Fachwissen für die 3DS-Runtime des Layton-Ports – IPC-Layout, Dienste (srv/APT/fs/gsp/cfg/dsp/hid/y2r/boss), Kernel-SVCs, GSP-Shared-Memory und Interrupts, PICA200-Register, RomFS/IVFC. Nutzen, wenn ein Dienst, SVC oder GPU-Befehl implementiert oder korrigiert wird.
---

# 3ds-hle: Dienste, Kernel, GPU richtig implementieren

Die belegten Fakten stehen in **`docs/REFERENCE.md`** – zuerst dort lesen.

## Vorgehen beim Implementieren eines Dienstes oder SVCs

1. Azahar-Implementierung als Spezifikation lesen (nur lesen, nichts linken):
   * Dienste: `tools/azahar/src/core/hle/service/<name>/*.cpp`
   * Kernel/SVC: `tools/azahar/src/core/hle/kernel/svc.cpp`, `mutex.cpp`,
     `semaphore.cpp`, `event.cpp`, `address_arbiter.cpp`, `thread.cpp`
   * GPU: `tools/azahar/src/video_core/pica/`, `shader/`, `rasterizer/`
   * Ergänzend: 3dbrew, libctru (siehe REFERENCE.md, Quellen).
2. Reply-Layout exakt nachbauen: Header-Wort, Result, Parameter, Deskriptoren.
   Im Port: Platzhalter-`put_result()` zuerst (siehe REFERENCE.md, IPC).
3. Unbekannte Kommandos **laut** melden (`[ipc] unbehandelt ...`), nicht
   still mit 0 beantworten.
4. Kein spielspezifischer Code in Diensten/Kernel: keine Spieladressen,
   keine Asset-Dateinamen, keine Sonderfälle für dieses Spiel.

## Wichtige Fallen (belegt)

* **IPC-Header-Slot**: `ipc_finish()` überschreibt `buf[0]`; ohne
  Platzhalterwort rutscht `Result` um ein Wort.
* **StaticBuffer**: Adresse ist das *zweite* Wort, Größe in Bits 14..31 des
  ersten.
* **SelfNCCH-RomFS**: Stream-Offset 0 = Level-3-Header (Image-Offset
  `0x1000`), nicht der IVFC-Header.
* **GSP-Interrupts**: Das Spiel wartet auf PDC0 (VBlank) und P3D
  (Kommandoliste fertig). Beide müssen in die InterruptRelayQueue
  geschrieben und das registrierte Event signalisiert werden.
* **PICA-Gruppen-Records** sind 8-Byte-ausgerichtet (Pad-Wort).
* **Thumb-Interworking**: Funktionszeiger mit Bit 0 = Thumb. Der Dispatcher
  muss `addr & ~1` nachschlagen und den Modus setzen; Spielcode deshalb nie
  patchen.

## Verdacht auf Recompiler-Fehler

Mehrere frühere Workarounds (RomFS-Hash-Lookup, `mblen`/Locale,
Namenslänge `strlen+1`) sehen nach Semantikfehlern aus: Das Spiel läuft auf
echter Hardware mit denselben Daten. Bevor ein Dienst „repariert“ wird, die
betroffene Gastfunktion isoliert prüfen (Eingaben dumpen, Ergebnis mit
Hand-/Referenzrechnung vergleichen; `python tests/mvp_selftest.py` um einen
Fall erweitern).
