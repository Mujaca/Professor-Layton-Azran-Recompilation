"""Baut die Port-Binary (out/port.exe) aus dem recompilierten Code.

  python tools/build_port.py
"""
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")


def prep_submit_seed():
    """0x12FD30 ist der reale GX-Submit (push {r4..fp,lr}; bl 0x139914).
    Ein frueherer Diagnose-Patch hatte ihn durch 'mov r0,#1; bx lr' ersetzt
    und damit SubmitCmdList/0x139914/0x13F2D4 komplett uebersprungen.
    Originalbytes wiederherstellen und die Adresse als Recompiler-Seed
    eintragen, damit fn_0012FD30 als eigene Funktion entsteht."""
    dpath = os.path.join(ROOT, "out", "exefs", "code_decomp.bin")
    d = bytearray(open(dpath, "rb").read())
    changed = False
    for addr, val in ((0x0012FD30, 0xE92D4FF0),   # push {r4-r11,lr}
                      (0x0012FD34, 0xE1A04000),   # mov r4,r0
                      (0x00124A14, 0xE1A04000),   # mov r4,r0 (Retprobe-Cave weg)
                      (0x00124A40, 0xE3500000),   # cmp r0,#0 (Retprobe-Cave weg)
                      (0x0010F8C4, 0xE3A00001),   # Menue-Loader-Gate: mov r0,#1
                      (0x0010F8C8, 0xE12FFF1E)):  # bx lr
        o = addr - 0x100000
        cur = struct.unpack_from("<I", d, o)[0]
        if cur != val:
            struct.pack_into("<I", d, o, val)
            print("%08X: %08X -> %08X (Originalpfad wiederhergestellt)" %
                  (addr, cur, val))
            changed = True
    if changed:
        open(dpath, "wb").write(d)

    spath = os.path.join(ROOT, "out", "seeds.txt")
    seeds = []
    if os.path.exists(spath):
        seeds = [x.strip() for x in open(spath, encoding="utf-8")
                 if x.strip()]
    if "0012FD30" not in [x.upper() for x in seeds]:
        seeds.append("0012FD30")
        open(spath, "w", encoding="utf-8", newline="\n").write(
            "\n".join(seeds) + "\n")
        print("Seed 0012FD30 -> out/seeds.txt ergaenzt")


def add_retforce_hooks(txt):
    """Generischer HLE_RETFORCE-Einstieg: jede generierte fn_XXXXXX prueft
    einmal arm_retforce(); bei Env-Treffer kehrt sie mit r0=<wert> zurueck.
    Ohne HLE_RETFORCE kostet das nur einen Zweig pro Funktionsaufruf."""
    if "arm_retforce(c," in txt:
        return txt
    pat = re.compile(r"(?m)^void fn_([0-9A-Fa-f]{8})\(ArmCpu \*c\) \{")
    return pat.sub(
        lambda m: "%s\n  if (arm_retforce(c, 0x%su)) return;"
                  % (m.group(0), m.group(1).upper()), txt)


def main():
    env = dict(os.environ)
    env["PYTHONPATH"] = TOOLS
    env["PYTHONIOENCODING"] = "utf-8"

    prep_submit_seed()

    gen = [sys.executable, "-m", "recomp.cli", "gen",
           "--in", os.path.join(ROOT, "out", "exefs", "code_decomp.bin"),
           "--out", os.path.join(ROOT, "build", "gen_boot.c"),
           "--prologues", "--seed-pointers", "--trace",
           "--seed-file", os.path.join(ROOT, "out", "seeds.txt")]
    p = subprocess.run(gen, cwd=ROOT, env=env, capture_output=True, text=True)
    print(p.stdout.strip() or p.stderr.strip()[-2000:])
    if p.returncode != 0:
        return p.returncode

    # Generischer Runtime-Force-Hook (HLE_RETFORCE) in alle fn_-Einstiege.
    gpath = os.path.join(ROOT, "build", "gen_boot.c")
    txt = open(gpath, encoding="utf-8").read()
    txt2 = add_retforce_hooks(txt)
    if txt2 is not txt:
        txt = txt2
        open(gpath, "w", encoding="utf-8", newline="\n").write(txt)
        print("HLE_RETFORCE-Hooks in gen_boot.c eingefuegt")

    # Nachbearbeitung: das SDK schickt Service-Namen mit strlen+1
    # (also 9 fuer "gsp::Gpu"), der Wrapper fn_0028EE00 akzeptiert
    # aber nur <= 8 und bricht sonst ohne Request ab.
    gpath = os.path.join(ROOT, "build", "gen_boot.c")
    txt = open(gpath, encoding="utf-8").read()
    # .fa-Streams direkt aus out/romfs/lt6/arc/lt6_*.fa bedienen: der
    # CFile-Stream liefert sonst nur 4 Bytes (Header/Index unvollstaendig).
    keyfa = "void fn_00280744(ArmCpu *c) {\n"
    if keyfa in txt and "hle_fa_read(c)" not in txt:
        txt = txt.replace(keyfa,
                          keyfa + "  if (hle_fa_read(c) >= 0) return;\n", 1)
    key = "void fn_0028EE00(ArmCpu *c) {"
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

    # HLE-Stub fuer den 2D-Displaylisten-Eintrag (0x0CFE1000) in die
    # Recompiler-Tabelle aufnehmen, damit der Engine-Renderer ihn aufruft.
    txt = open(gpath, encoding="utf-8").read()
    if "hle_2d_stub" not in txt:
        mark = "static const ArmEntry g_entries[] = {"
        i = txt.find(mark)
        if i >= 0:
            j = txt.find("};", i)
            seg = txt[i:j].rstrip()
            if not seg.endswith(","):
                seg += ","
            seg += "\n    { 0x0CFE1000u, hle_2d_stub },\n"
            txt = (txt[:i] + "extern void hle_2d_stub(ArmCpu *c);\n" +
                   seg + txt[j:])
            open(gpath, "w", encoding="utf-8", newline="\n").write(txt)

    # HLE-Stub "return 1" (fuer synthetische Vtable-Open-Funktionen).
    txt = open(gpath, encoding="utf-8").read()
    if "hle_ret_one" not in txt:
        mark = "static const ArmEntry g_entries[] = {"
        i = txt.find(mark)
        if i >= 0:
            j = txt.find("};", i)
            seg = txt[i:j].rstrip()
            if not seg.endswith(","):
                seg += ","
            seg += "\n    { 0x0CFE1200u, hle_ret_one },\n"
            txt = (txt[:i] + "extern void hle_ret_one(ArmCpu *c);\n" +
                   seg + txt[j:])
            open(gpath, "w", encoding="utf-8", newline="\n").write(txt)

    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), "zig.exe")
    cmd = [zig, "cc", "-std=c11", "-O1",
           "-I", os.path.join(ROOT, "runtime"),
           "-I", os.path.join(ROOT, "runtime", "hle"),
           "-I", os.path.join(ROOT, "runtime", "platform"),
           os.path.join(ROOT, "tests", "port.c"),
           os.path.join(ROOT, "build", "gen_boot.c"),
           os.path.join(ROOT, "runtime", "arm_cpu.c"),
           os.path.join(ROOT, "runtime", "arm_trace.c"),
    os.path.join(ROOT, "runtime", "fa_index.c"),
           os.path.join(ROOT, "runtime", "gpu.c"),
           os.path.join(ROOT, "runtime", "imgc.c"),
           os.path.join(ROOT, "runtime", "hle", "hle.c"),
           os.path.join(ROOT, "runtime", "platform", "window.c"),
           "-o", os.path.join(ROOT, "out", "port.exe"),
           "-luser32", "-lgdi32"]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stderr[-3000:])
        return p.returncode
    print("gebaut: out/port.exe")
    return 0


if __name__ == "__main__":
    sys.exit(main())
