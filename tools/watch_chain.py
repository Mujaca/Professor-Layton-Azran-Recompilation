"""guest-watch mit dem benannten Profil (tools/profiles/chain.json).

  python tools/watch_chain.py --write 0x42B888 --timeout 45 --label poolcnt

Beobachtet Schreibzugriffe auf Ring-/Pool-Felder des Node-Builders.
"""
import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", default=os.path.join(ROOT, "tools", "profiles",
                                                      "chain.json"))
    ap.add_argument("--write")
    ap.add_argument("--read")
    ap.add_argument("--range")
    ap.add_argument("--pc")
    ap.add_argument("--timeout", default="45")
    ap.add_argument("--label", default="chainwatch")
    ap.add_argument("--env", action="append",
                    help="Profil-Env ueberschreiben (NAME=WERT, mehrfach)")
    ap.add_argument("--hook", action="append",
                    help="HLE_HOOK-Kurzform: ADDR=KIND[,ADDR=KIND]")
    ap.add_argument("--hook-max", default=None)
    args = ap.parse_args(argv)

    prof = json.load(open(args.profile, encoding="utf-8"))
    cmd = [sys.executable, os.path.join(ROOT, "tools", "g3ds.py"),
           "guest-watch", "--timeout", args.timeout, "--label", args.label,
           "--exe", "out\\port.exe",
           "--arg", "out\\romfs",
           "--arg=--shot",
           "--arg", "out\\shot_watch.bmp"]
    if args.write:
        cmd += ["--write", args.write]
    if args.read:
        cmd += ["--read", args.read]
    if args.range:
        cmd += ["--range", args.range]
    if args.pc:
        cmd += ["--pc", args.pc]
    for k, v in prof["env"].items():
        cmd += ["--env", "%s=%s" % (k, v)]
    for e in args.env or []:
        cmd += ["--env", e]
    if args.hook:
        cmd += ["--env", "HLE_HOOK=" + ",".join(args.hook)]
    if args.hook_max:
        cmd += ["--env", "HLE_HOOK_MAX=" + args.hook_max]
    print("watch:", " ".join(cmd[:8]), "...")
    return subprocess.call(cmd, cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
