---
name: 3ds-port
description: Mess- und Analyse-Rezepte fuer den statischen 3DS-Port (Professor Layton/Azran Legacy). Nutze diesen Skill fuer Build, Boot, Screenshot, Log-Auswertung, Meilensteine, callgraph/where/diff_trace/matrix/perfmap/hotpatch/framecheck und den Fallstricken-Katalog. Die vollstaendige Fassung steht in docs/SKILL.md.
---

# 3ds-port: Messrezepte

Vollstaendige Anleitung: [`docs/SKILL.md`](../../../docs/SKILL.md).
Naechster Auftrag: [`docs/HANDOVER_PROMPT.md`](../../../docs/HANDOVER_PROMPT.md).

Schnellstart (kanonischer Engine-PICA-Lauf):

```powershell
python tools\build_port.py
python tests\mvp_selftest.py
python tools\selftest_tools.py
python tools\run_chain_direct.py --profile tools\profiles\engine_pica_draw.json --shot out\shot_tmp.bmp
python tools\canon.py --log out\logs\chain_direct.txt --shot out\shot_tmp.bmp --engine
python tools\measure_ref.py out\shot_engine.bmp
```

Aktueller Stand (Commit `1edae74`, 2026-11-23, verifiziert): TOP sichtbar
`69370/90400` mean `47.98`; BOTTOM sichtbar `25902/71360` mean `20.57`
(`HLE_PICA_CLEAR=1`, zwei Laeufe identisch). Der Draw-Block stammt aus dem
Engine-Strom @`0x30` -> `0x0CFE6000`, aber Quads/Vertex-Attribute und
Texturen emittiert noch HLE (`HLE_PICA_SPRITES`). Die Scene-Paar-Kette
`11A3E0 -> 27C2BC -> 2794A4 -> 2721E0` laeuft (Profil
`tools/profiles/scene_pairs.json`). Adresskorrektur: die Paare liegen bei
`ctx+0xB34/0xB3C` und sind **PICA-Paare**, keine Float-UVs (UVs im
Desc-Buffer `[ctx+0xD6C]`). Offen: 0xD2-Container-Link und natuerlicher
Submit. Details:
[`docs/PORT_STATE.md`](../../../docs/PORT_STATE.md) (letzte Abschnitte).

Wichtigste Fallstricke: `getenv()` nie im Hot Path (static cachen),
IPC-Reply braucht ein Dummy-Wort vor `Result`, `GetTickCount64()` hat
~15,6 ms Aufloesung, `HLE_TICK=0` stoppt die Tick-Tasks, RomFS-IVFC-Groessen
stehen bei `0x10/0x18/0x20`, SelfNCCH ist Archive-ID 3, `tools/decomp.py`
ueberschreibt alle Patches, `arm_trace_block` existiert nur an
Blockanfaengen. Bildmessung **nur** mit `HLE_PICA_CLEAR=1` (sonst akkumuliert
Alpha ueber die Frames: 65707 vs 85271), `HLE_PRESENT_FILL=0` fuer echte
Render-Nachweise, `measure_ref.py` statt `_cmp_shot.py` fuer den
400x480-Shot.

Prozessregel: nach jedem belegten Fakt eine Zeile hier **und** in
`3ds-hle` ergaenzen, nicht nur in `docs/PORT_STATE.md`.
