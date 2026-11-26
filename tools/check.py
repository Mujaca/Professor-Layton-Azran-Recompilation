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
    ap.add_argument("--min-services", type=int, default=None,
                    help="Mindestwert statt letzter ROADMAP-Zeile (nur fuer bewusst erlaubte Rueckschritte)")
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
    # Runtime-Selbsttests (C): frueher nicht im Gate -> ein HW-Renderer-Commit
    # mit 16 roten hw_test-Faellen wurde angenommen (2026-11-25). Der
    # HW-Test laeuft auf WARP, damit er ohne Grafikkarte reproduzierbar ist.
    for t in ["dsp_hle_test", "audio_test", "y2r_test", "framebuffer_test",
              "fs_archive_test", "frame_clock_test", "sched_selftest", "hid_ring_test",
              "lighting_test", "vs_test", "romfs_overlay_test", "lua_host_test"]:
        step("Runtime-Selbsttest " + t, [py, "tests/%s.py" % t], timeout=300)
    step("HW-Renderer-Selbsttest (WARP)", [py, "tests/hw_test.py", "--warp"], timeout=600)
    need = roadmap_services() if a.min_services is None else a.min_services
    step("Frontier >= %d Dienste" % need, [py, "tools/frontier.py", "--min-services", str(need)])
    print("OK")


if __name__ == "__main__":
    main()
