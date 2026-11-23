"""Ruft den Orchestrator 0x12493C direkt auf (Emitter+Submit) mit
erzwungenen Gates und Scratch-Allokation, und prueft qops.

Kette: 0x12493C -> 0x12FC24 (Emitter) -> 0x12FD30 -> 0x139914 -> 0x13F2D4.

Profil: tools/profiles/chain.json (alle Env-Schalter an einer Stelle).

  python tools/run_chain_direct.py [--profile tools/profiles/chain.json]
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile",
                    default=os.path.join(ROOT, "tools", "profiles", "chain.json"))
    ap.add_argument("--shot", default="out\\shot_chain_direct.bmp")
    ap.add_argument("--log", default=os.path.join(
        ROOT, "out", "logs", "chain_direct.txt"),
        help="Logpfad (parallel arbeitende Sessions: eigener Pfad)")
    args = ap.parse_args(argv)

    prof = json.load(open(args.profile, encoding="utf-8"))
    env = dict(os.environ)
    env.update(prof["env"])

    log = args.log
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", args.shot], cwd=ROOT, env=env,
                       capture_output=True, text=True, errors="replace")
    text = (p.stdout or "") + (p.stderr or "")
    open(log, "w", encoding="utf-8", newline="\n").write(text)
    print("Log:", log)
    for addr, name in (("00139914", "139914"), ("0013F2D4", "13F2D4"),
                       ("0012FD30", "12FD30")):
        h = re.findall(r"\[hook-final\].*addr=" + addr + r".*", text)
        print(("OK   " + h[0]) if h else ("FEHLT " + name))
    for line in text.splitlines():
        if "gspq" in line or "qops=" in line or "postemit" in line:
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
