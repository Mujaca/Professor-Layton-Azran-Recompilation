"""Textersetzungen im generierten C (build/gen_boot.c).

Technische Schuld (docs/WORKAROUNDS.md). Jede Ersetzung ist spiel-
spezifisch und verdeckt vermutlich einen Fehler in Recompiler, Kernel oder
Services. Ziel: diese Datei loeschen koennen.

Unveraendert aus dem frueheren tools/build_port.py uebernommen.
"""


def apply(gpath):
    # Nachbearbeitung: das SDK schickt Service-Namen mit strlen+1
    # (also 9 fuer "gsp::Gpu"), der Wrapper fn_0028EE00 akzeptiert
    # aber nur <= 8 und bricht sonst ohne Request ab.
    txt = open(gpath, encoding="utf-8").read()
    key ="void fn_0028EE00(ArmCpu *c) {"
    i = txt.find(key)
    if i >= 0:
        j = txt.find("\nvoid fn_", i + len(key))
        if j < 0:
            j = i + 4000
        seg = txt[i:j]
        seg = seg.replace("DP_CMP, -1, c->r[2], 0x00000008u",
                          "DP_CMP, -1, c->r[2], 0x00000009u")
        txt = txt[:i] + seg + txt[j:]
        open(gpath, "w", encoding="utf-8", newline="\n").write(txt)
        # Datei-Open: fn_001187F8 bricht ab, wenn 0x27E2E0/0x27E1FC 0
        # liefern (Rueckgabe dann 0xD8604659). Abzweige entschaerfen.
        txt = open(gpath, encoding="utf-8").read()
        key2 = "void fn_001187F8(ArmCpu *c) {"
        i2 = txt.find(key2)
        if i2 >= 0:
            j2 = txt.find("\nvoid fn_", i2 + len(key2))
            if j2 < 0:
                j2 = i2 + 8000
            seg2 = txt[i2:j2].replace("goto L_001187F8_001188B0;", ";")
            txt = txt[:i2] + seg2 + txt[j2:]
        # Diagnose: den Rueckgabewert des "rom:"-Datei-Opens als Erfolg
        # behandeln (sonst wirft fn_00107AE4 D8604659).
        txt = open(gpath, encoding="utf-8").read()
        key3 = "fn_001187F8(c);\n    arm_restore_sp(c, sp0); }\n  if (c->aborted) return;"
        if key3 in txt:
            txt = txt.replace(key3, key3 + "\n  c->r[0] = 0u;   /* Datei-Open = Erfolg */", 1)
            open(gpath, "w", encoding="utf-8", newline="\n").write(txt)
        # Asset-Registrierung: fn_00100F98 bricht bei 0x101370 ab, wenn
        # fn_001034F4 0 liefert (Flag [sb+0x567] aus dem Save ist bei uns 0).
        # Den Zweig auf den Asset-Block 0x1013F0 umbiegen.
        txt = open(gpath, encoding="utf-8").read()
        key4 = "void fn_00100F98(ArmCpu *c) {"
        i4 = txt.find(key4)
        if i4 >= 0:
            j4 = txt.find("\nvoid fn_", i4 + len(key4))
            if j4 < 0:
                j4 = i4 + 30000
            seg4 = txt[i4:j4]
            bad = ("    fn_001034F4(c);\n    arm_restore_sp(c, sp0); }\n"
                   "  if (c->aborted) return;\n"
                   "  arm_dp(c, DP_MOV, 4, 0, c->r[0], 1, -1);\n"
                   "  if (arm_cond(c, 0)) {\n"
                   "    goto L_00100F98_001015D0;\n")
            good = ("    fn_001034F4(c);\n    arm_restore_sp(c, sp0); }\n"
                    "  if (c->aborted) return;\n"
                    "  c->r[4] = 1u;   /* Menue-Setup-Zweig (r4 != 0) */\n"
                    "  if (arm_cond(c, 14)) {   /* Asset-Registrierung erzwingen */\n"
                    "    goto L_00100F98_001013F0;\n")
            if bad in seg4:
                seg4 = seg4.replace(bad, good)
            txt = txt[:i4] + seg4 + txt[j4:]
            open(gpath, "w", encoding="utf-8", newline="\n").write(txt)

            open(gpath, "w", encoding="utf-8", newline="\n").write(txt)

    # Recompiler-Einstieg 0x1ED64C fehlt: Das ist der Prolog des
    # 2D-Renderer-Init, sein Rumpf ist fn_001ED65C. Der Gast ruft 0x1ED64C
    # (Block 0x00285C48); ohne Tabelleneintrag bricht der CPU-Thread mit
    # "arm: unbekanntes call-Ziel 0x001ED64C" ab (pc=00100000).
    txt = open(gpath, encoding="utf-8").read()
    if "fn_001ED64C" not in txt and "void fn_001ED65C(ArmCpu *c);" in txt:
        txt = txt.replace("void fn_001ED65C(ArmCpu *c);",
                          "void fn_001ED64C(ArmCpu *c);\n"
                          "void fn_001ED65C(ArmCpu *c);", 1)
        tbl = "{ 0x001ED65Cu, fn_001ED65C }"
        if tbl in txt:
            txt = txt.replace(tbl,
                              "{ 0x001ED64Cu, fn_001ED64C },\n    " + tbl, 1)
        defn = "void fn_001ED65C(ArmCpu *c) {"
        wrapper = (
            "void fn_001ED64C(ArmCpu *c) {\n"
            "  /* 0x1ED64C: frueher Ausstieg, sonst Rumpf ab 0x1ED65C. */\n"
            "  c->r[2] = arm_ld(c, c->r[0] + 0x0000020Cu, 4, 0);\n"
            "  if (c->r[2] != 0u) { c->r[0] = 0u; return; }\n"
            "  fn_001ED65C(c);\n"
            "}\n\n")
        if defn in txt:
            txt = txt.replace(defn, wrapper + defn, 1)
        open(gpath, "w", encoding="utf-8", newline="\n").write(txt)

    # GPU-Busy-Warteschleife in fn_001B9938 (Block 0x1BA898) entfernen:
    # Der Port rendert synchron, der Renderer darf nach der ersten
    # Pruefung sofort zum Finalisieren (0x1BA8A8) gehen.
    txt = open(gpath, encoding="utf-8").read()
    key = "L_001B9938_001BA898:"
    i = txt.find(key)
    if i >= 0:
        j = txt.find("\nL_001B9938_", i + len(key))
        if j < 0:
            j = i + 1200
        seg = txt[i:j]
        old = "goto L_001B9938_001BA364;"
        if old in seg:
            seg = seg.replace(old, "goto L_001B9938_001BA8A8;", 1)
            txt = txt[:i] + seg + txt[j:]
            open(gpath, "w", encoding="utf-8", newline="\n").write(txt)

    return 0
