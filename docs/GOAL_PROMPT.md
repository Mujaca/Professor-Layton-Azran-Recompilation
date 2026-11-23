# GOAL PROMPT (copy/paste)

Arbeite im Repo `C:\Users\user\Documents\Repos\3DS` am statischen 3DS-Port.
Lies zuerst `docs/HANDOVER_PROMPT.md`, `docs/SKILL.md` und die letzten
Einträge in `docs/PORT_STATE.md`, lade die Skills **3ds-hle** und
**3ds-port** und starte 2–3 parallele Sub-Agents nach der
Handoff-Arbeitsweise. Treibe dann den nächsten Schritt: den
engine-emittierten PICA-Strom @`0x30` (inkl. `0x22F`-Draw) so in den
HLE-PICA-Renderer bzw. nach `0x0CFE6000` bringen, dass
`out/shot_engine.bmp` das echte Menü zeigt. Belege jeden Fakt mit
Log/Test/Dump, halte die kanonischen Dateinamen (`tools/canon.py`) ein
und committe pro belegtem Fakt.
