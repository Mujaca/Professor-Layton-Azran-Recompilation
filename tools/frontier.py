"""Frontier-Metrik: wie weit kommt der natürliche Boot im Vergleich zu Azahar?

  python tools/frontier.py                 # out/port.exe, 40 s
  python tools/frontier.py --timeout 90 --json out/logs/frontier.json
  python tools/frontier.py --log out/logs/frontier.txt   # vorhandenes Log auswerten

Startet die Port-Binary ohne Verhaltens-Schalter (nur HLE_LOG=1), vergleicht
die geöffneten Dienste mit der Referenzfolge aus reports/azahar_trace.txt und
prüft die Meilensteine aus docs/ROADMAP.md. Das Ergebnis ist die einzige
Fortschrittszahl des Projekts.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from g3ds import trace_diff  # noqa: E402

# Umgebungsvariablen, die Gastverhalten ändern könnten, werden entfernt.
_ALLOWED_ENV_PREFIX = ("HLE_LOG", "HLE_TRACE")

MILESTONES = [
    ("M2", "Spiel ruft SetBufferSwap", re.compile(r"^\[gsp\] SetBufferSwap", re.M)),
    ("M3", "Spiel schickt PICA-Liste mit Draws", re.compile(r"^\[gpu\] list .* draws=[1-9]", re.M)),
]


def run_port(exe: Path, timeout: int, log: Path) -> int:
    env = {k: v for k, v in os.environ.items()
           if not (k.startswith("HLE_") or k.startswith("PORT_") or k.startswith("ARM_"))
           or k.startswith(_ALLOWED_ENV_PREFIX)}
    env["HLE_LOG"] = "1"
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("wb") as f:
        try:
            p = subprocess.run([str(exe), str(ROOT / "out" / "romfs"), "--renderer", "soft"], cwd=str(ROOT),
                               env=env, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
            return p.returncode
        except subprocess.TimeoutExpired:
            return -1  # erwartet: das Spiel läuft weiter


def evaluate(log: Path) -> dict:
    ref = trace_diff.parse_reference(ROOT / "reports" / "azahar_trace.txt")
    text = log.read_text(encoding="utf-8", errors="replace")
    try:
        port = trace_diff.parse_port(log)
        cmp = trace_diff.compare(ref, port, "srv")
        ref_services = cmp["ref_services"]
        port_lower = {s.lower() for s in port.services}
        reached = 0
        for s in ref_services:
            if s.lower() not in port_lower:
                break
            reached += 1
        first = cmp["first"]
    except SystemExit:
        ref_services, reached, first = [], 0, "keine Dienstaufrufe im Log"
    patches = json.load((ROOT / "tools" / "workarounds" / "code_patches.json")
                        .open(encoding="utf-8"))["patches"]
    ms = {"M1": bool(ref_services) and reached == len(ref_services)}
    for key, _, rx in MILESTONES:
        ms[key] = bool(rx.search(text))
    return {
        "services": reached,
        "services_total": len(ref_services),
        "first_divergence": first,
        "milestones": ms,
        "workarounds": len(patches),
        "log": str(log),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--exe", default=str(ROOT / "out" / "port.exe"))
    ap.add_argument("--timeout", type=int, default=40)
    ap.add_argument("--log", help="vorhandenes Log auswerten statt neu zu laufen")
    ap.add_argument("--json", help="Ergebnis zusätzlich als JSON schreiben")
    ap.add_argument("--min-services", type=int, default=None,
                    help="Exit-Code 1, wenn weniger Dienste erreicht werden")
    a = ap.parse_args()

    if a.log:
        log = Path(a.log)
    else:
        log = ROOT / "out" / "logs" / "frontier.txt"
        run_port(Path(a.exe), a.timeout, log)
    res = evaluate(log)
    print("services   %d/%d" % (res["services"], res["services_total"]))
    print("divergenz  %s" % res["first_divergence"])
    print("meilenst.  %s" % " ".join("%s=%s" % (k, "ja" if v else "nein")
                                      for k, v in res["milestones"].items()))
    print("patches    %d" % res["workarounds"])
    if a.json:
        Path(a.json).write_text(json.dumps(res, indent=1, ensure_ascii=False), encoding="utf-8")
    if a.min_services is not None and res["services"] < a.min_services:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
