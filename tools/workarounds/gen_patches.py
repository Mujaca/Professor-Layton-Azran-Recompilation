"""Textersetzungen im generierten C (build/gen_boot.c).

Technische Schuld (docs/WORKAROUNDS.md). Ziel: diese Datei loeschen koennen.

Aktuell leer. Die fruehere Ersetzung in fn_0028EE00 (srv-Namenslaenge
`cmp r2,#8` -> `#9`) war unnoetig: `fn_0028F5E8` (strlen) liefert im Port
korrekt 8 fuer "gsp::Gpu"/"hid:USER" (Beleg: [srvname] len=8, Test
tests/mvp_selftest.py::strlen_wortschleife). Ursache der alten Abweichung war
der fehlende post-index-Writeback bei `ldr r1,[r0],#4` im Recompiler, der
inzwischen erzeugt wird.
"""


def apply(gpath):
    return 0
