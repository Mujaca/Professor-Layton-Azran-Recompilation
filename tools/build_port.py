"""Baut die Port-Binary (out/port.exe) aus dem recompilierten Code.

  python tools/build_port.py
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")


def main():
    env = dict(os.environ)
    env["PYTHONPATH"] = TOOLS
    env["PYTHONIOENCODING"] = "utf-8"

    gen = [sys.executable, "-m", "recomp.cli", "gen",
           "--in", os.path.join(ROOT, "out", "exefs", "code_decomp.bin"),
           "--out", os.path.join(ROOT, "build", "gen_boot.c"),
           "--prologues", "--seed-pointers", "--trace",
           "--seed-file", os.path.join(ROOT, "out", "seeds.txt")]
    p = subprocess.run(gen, cwd=ROOT, env=env, capture_output=True, text=True)
    print(p.stdout.strip() or p.stderr.strip()[-2000:])
    if p.returncode != 0:
        return p.returncode

    # Nachbearbeitung: das SDK schickt Service-Namen mit strlen+1
    # (also 9 fuer "gsp::Gpu"), der Wrapper fn_0028EE00 akzeptiert
    # aber nur <= 8 und bricht sonst ohne Request ab.
    gpath = os.path.join(ROOT, "build", "gen_boot.c")
    txt = open(gpath, encoding="utf-8").read()
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
