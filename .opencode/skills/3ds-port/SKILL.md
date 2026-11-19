---
name: 3ds-port
description: Mess- und Analyse-Rezepte fuer den statischen 3DS-Port (Professor Layton/Azran Legacy). Nutze diesen Skill fuer Build, Boot, Screenshot, Log-Auswertung, Meilensteine, callgraph/where/diff_trace/matrix/perfmap/hotpatch/framecheck und den Fallstricken-Katalog. Die vollstaendige Fassung steht in docs/SKILL.md.
---

# 3ds-port: Messrezepte

Vollstaendige Anleitung: [`docs/SKILL.md`](../../../docs/SKILL.md).

Schnellstart:

```powershell
python tools\build_boot.py
python tools\where.py --run --thread 2
python tools\callgraph.py writers 0x40B4C4
python tools\diff_trace.py --port tools\fixtures\port_boot_20261119.log
python tools\milestones.py --log tools\fixtures\port_boot_20261119.log
python tools\framecheck.py
```

Wichtigste Fallstricke: `getenv()` nie im Hot Path, IPC-Reply braucht ein
Dummy-Wort vor `Result`, `GetTickCount64()` hat ~15,6 ms Aufloesung,
`HLE_TICK=0` stoppt die Tick-Tasks, RomFS-IVFC-Groessen stehen bei
`0x10/0x18/0x20`, SelfNCCH ist Archive-ID 3.
