"""Einmal-Lauf mit Profil-Env + Extras; druckt [hook]/[watch]-Zeilen.

  python tools/_objdump_run.py --hook 28BE0C=mem --hook 28AB04=regs
"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", default=os.path.join(ROOT, "tools", "profiles",
                                                      "chain.json"))
    ap.add_argument("--hook", action="append", default=[])
    ap.add_argument("--hook-max", default="3")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--grep", default="hook")
    args = ap.parse_args(argv)

    prof = json.load(open(args.profile, encoding="utf-8"))
    env = dict(os.environ)
    env.update(prof["env"])
    for e in args.env:
        k, v = e.split("=", 1)
        env[k] = v
    if args.hook:
        env["HLE_HOOK"] = ",".join(args.hook)
        env["HLE_HOOK_MAX"] = args.hook_max
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", "out\\shot_objdump.bmp"], cwd=ROOT, env=env,
                       capture_output=True, text=True, errors="replace")
    text = (p.stdout or "") + (p.stderr or "")
    out = os.path.join(ROOT, "out", "logs", "objdump.txt")
    open(out, "w", encoding="utf-8", newline="\n").write(text)
    for line in text.splitlines():
        if args.grep in line:
            print(line)
    print("Log:", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
