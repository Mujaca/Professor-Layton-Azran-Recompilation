"""Gate vor jedem Commit.

  python tools/check.py            # Tests + Build (Runtime) + Frontier
  python tools/check.py --full     # inkl. kompletter Neugenerierung des C-Codes
  python tools/check.py --quick    # nur Tests, kein Build/Lauf

Frontier darf nicht unter den letzten Eintrag in docs/ROADMAP.md fallen.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def step(name, cmd, timeout=1800):
    print("==", name)
    p = subprocess.run(cmd, cwd=str(ROOT), timeout=timeout)
    if p.returncode != 0:
        print("FEHLGESCHLAGEN:", name)
        sys.exit(1)


def roadmap_services():
    text = (ROOT / "docs" / "ROADMAP.md").read_text(encoding="utf-8")
    rows = re.findall(r"^\|\s*\d{4}-\d{2}-\d{2}\s*\|[^|]*\|\s*(\d+)/\d+\s*\|", text, re.M)
    return int(rows[-1]) if rows else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--quick", action="store_true")
    a = ap.parse_args()
    py = sys.executable
    step("Recompiler-Semantik", [py, "tests/mvp_selftest.py"])
    step("IMGC-Dekoder", [py, "tools/test_imgc_c.py"])
    step("PICA-Registermap", [py, "tools/test_pica_c.py"])
    if a.quick:
        print("OK (quick)")
        return
    step("Build", [py, "tools/build.py"] + ([] if a.full else ["--no-gen"]))
    step("Tool-Selbsttest", [py, "tools/selftest_tools.py"])
    need = roadmap_services()
    step("Frontier >= %d Dienste" % need, [py, "tools/frontier.py", "--min-services", str(need)])
    print("OK")


if __name__ == "__main__":
    main()
