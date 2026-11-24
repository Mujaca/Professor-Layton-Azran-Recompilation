"""Textersetzungen im generierten C (build/gen_boot.c).

Technische Schuld (docs/WORKAROUNDS.md). Ziel: diese Datei loeschen koennen.

Aktuell verbleibend: srv-Namenslaenge. Das Spiel schickt strlen+1
(9 fuer "gsp::Gpu"); der SDK-Wrapper fn_0028EE00 prueft auf <= 8.
Test: alle anderen Ersetzungen wurden entfernt (Frontier bleibt 9/10).
"""


def apply(gpath):
    txt = open(gpath, encoding="utf-8").read()
    key = "void fn_0028EE00(ArmCpu *c) {"
    i = txt.find(key)
    if i >= 0:
        j = txt.find("\nvoid fn_", i + len(key))
        if j < 0:
            j = i + 4000
        seg = txt[i:j].replace("DP_CMP, -1, c->r[2], 0x00000008u",
                               "DP_CMP, -1, c->r[2], 0x00000009u")
        txt = txt[:i] + seg + txt[j:]
        open(gpath, "w", encoding="utf-8", newline="\n").write(txt)
    return 0
