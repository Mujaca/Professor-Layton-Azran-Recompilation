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
