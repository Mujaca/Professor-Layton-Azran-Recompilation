"""Baut out/boot.exe aus dem vorhandenen build/gen_boot.c (ohne Neugenerierung).

  python tools/build_boot.py            # nur Runtime neu uebersetzen
  python tools/build_boot.py --regen    # vorher build_port.py ausfuehren

Fuer schnelle Instrumentierungs-Iterationen: gen_boot.c bleibt unveraendert,
nur die Runtime (arm_trace.c/hle.c) wird neu kompiliert.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="build_boot")
    p.add_argument("--regen", action="store_true",
                   help="vorher python tools/build_port.py ausfuehren")
    p.add_argument("--out", help="Default: out/boot.exe bzw. out/port.exe")
    p.add_argument("--opt", default="-O1")
    p.add_argument("--port", action="store_true",
                   help="tests/port.c + window.c statt tests/boot.c bauen")
    args = p.parse_args(argv)
    if not args.out:
        args.out = str(ROOT / "out" / ("port.exe" if args.port else "boot.exe"))

    if args.regen:
        r = subprocess.run([sys.executable, str(TOOLS / "build_port.py")],
                           cwd=str(ROOT))
        if r.returncode != 0:
            return r.returncode

    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), "zig.exe")
    cmd = [
        zig, "cc", "-std=c11", args.opt,
        "-I", str(ROOT / "runtime"),
        "-I", str(ROOT / "runtime" / "hle"),
        "-I", str(ROOT / "runtime" / "platform"),
        str(ROOT / "tests" / ("port.c" if args.port else "boot.c")),
        str(ROOT / "build" / "gen_boot.c"),
        str(ROOT / "runtime" / "arm_cpu.c"),
        str(ROOT / "runtime" / "arm_trace.c"),
        str(ROOT / "runtime" / "fa_index.c"),
        str(ROOT / "runtime" / "gpu.c"),
        str(ROOT / "runtime" / "imgc.c"),
        str(ROOT / "runtime" / "hle" / "hle.c"),
    ]
    if args.port:
        cmd.append(str(ROOT / "runtime" / "platform" / "window.c"))
        cmd.extend(["-luser32", "-lgdi32"])
    cmd.extend(["-o", args.out])
    print("baue %s ..." % args.out)
    r = subprocess.run(cmd, cwd=str(ROOT))
    if r.returncode != 0:
        print("Build fehlgeschlagen", file=sys.stderr)
        return r.returncode
    print("gebaut: %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
