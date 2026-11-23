"""Paket 6 (Ziel C): Lauf eines Submit-Profils ohne HLE_FRAME_LOOP2-Zwang.

Unterschied zu tools/run_chain_direct.py:
  * HLE_FRAME_LOOP2 darf fehlen (Profil steuert den Pfad selbst).
  * Log geht nach tools/out/submit_<label>.txt (kein out/logs-Kanon).
  * Es werden nur die fuer Ziel C relevanten Zeilen zusammengefasst.

  python tools\\submit_run.py --profile tools\\profiles\\submit_abort_probe.json --label abort_probe
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

WANT = ("[mount]", "[hook]", "[retforce]", "[frameloop2]", "[mrd]",
        "[gspq", "qops=", "[gsp] Trigger", "[mrd-final]", "[retforce-final]",
        "hook-final", "[q]", "[qs]", "[precall]")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", required=True)
    ap.add_argument("--label", default="run")
    ap.add_argument("--shot", default=None)
    ap.add_argument("--print-all", action="store_true")
    args = ap.parse_args(argv)

    prof = json.load(open(args.profile, encoding="utf-8"))
    env = dict(os.environ)
    env.update({k: str(v) for k, v in prof["env"].items()})
    shot = args.shot or os.path.join("tools", "out", "submit_%s.bmp" % args.label)

    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", shot], cwd=ROOT, env=env,
                       capture_output=True, text=True, errors="replace")
    text = (p.stdout or "") + (p.stderr or "")
    log = os.path.join(ROOT, "tools", "out", "submit_%s.txt" % args.label)
    os.makedirs(os.path.dirname(log), exist_ok=True)
    open(log, "w", encoding="utf-8", newline="\n").write(text)
    print("Log:", log)

    for line in text.splitlines():
        if args.print_all or any(w in line for w in WANT):
            print(line)

    for a in ("0012493C", "0012FC24", "0012FD30", "00139914", "0013F2D4",
              "0026F130", "002808A8", "00268C80"):
        h = re.findall(r"\[hook-final\].*addr=" + a + r".*", text)
        if h:
            print("HOOK", h[0])
    q = re.findall(r"\[gspq-engine\].*", text)
    for line in q:
        print("Q   ", line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
