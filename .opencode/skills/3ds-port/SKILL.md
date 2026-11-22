---
name: 3ds-port
description: Mess- und Analyse-Rezepte fuer den statischen 3DS-Port (Professor Layton/Azran Legacy). Nutze diesen Skill fuer Build, Boot, Screenshot, Log-Auswertung, Meilensteine, callgraph/where/diff_trace/matrix/perfmap/hotpatch/framecheck und den Fallstricken-Katalog. Die vollstaendige Fassung steht in docs/SKILL.md.
---

# 3ds-port: Messrezepte

Vollstaendige Anleitung: [`docs/SKILL.md`](../../../docs/SKILL.md).
Naechster Auftrag: [`docs/HANDOVER_PROMPT.md`](../../../docs/HANDOVER_PROMPT.md).

Schnellstart:

```powershell
python tools\build_port.py
python tools\_make_menu_frame.py
.\out\port.exe out\romfs --shot out\shot.bmp
python tools\_cmp_shot.py out\shot.bmp reports\menu_official.bin
python tests\mvp_selftest.py
```

Aktueller Stand: Der Top-Screen des Hauptmenues wird ueber den echten
gsp-Pfad gerendert (`SubmitCmdList`-Ring + `SetCommandList` 0x0006 +
`TriggerCmdReqQueue` 0x000C + `SetBufferSwap` 0x0005, `runtime/gpu.c` mit
`BlitRGB` 0x07); Nachweis 0 Pixel Diff. Naechstes Ziel: beide Screens
(400x240 + 320x240) uebereinander und Engine-eigener Render.

Wichtigste Fallstricke: `getenv()` nie im Hot Path (static cachen),
IPC-Reply braucht ein Dummy-Wort vor `Result`, `GetTickCount64()` hat
~15,6 ms Aufloesung, `HLE_TICK=0` stoppt die Tick-Tasks, RomFS-IVFC-Groessen
stehen bei `0x10/0x18/0x20`, SelfNCCH ist Archive-ID 3, `tools/decomp.py`
ueberschreibt alle Patches, `arm_trace_block` existiert nur an
Blockanfaengen.
