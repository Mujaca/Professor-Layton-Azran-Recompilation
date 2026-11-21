"""Spielt den dokumentierten Patch-Satz reproduzierbar auf code_decomp.bin auf.

Reihenfolge ist wichtig (Code-Caves duerfen sich nicht ueberlappen):

  0x399F00..0x399F33  patch_narrowcopy.py
  0x399F40..0x399F4B  patch_size_stub.py
  0x399F60..0x399F74  patch_retprobe.py  (optional, Diagnose)
  0x399F80..0x399F93  patch_retprobe.py
  0x399FA0..0x399FBB  patch_widecopy.py
  0x399FE8..0x399FFB  patch_empty_component.py

Ohne Argument werden alle produktiven Patches angewendet; `--no-retprobe`
laesst die Diagnose-Caves weg.
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ORDER = [
    "patch_interwork_2927b8.py",
    "patch_lang_uk.py",
    "patch_chain_end.py",
    "patch_empty_component.py",
    "patch_widecopy.py",
    "patch_narrowcopy.py",
    "patch_mblen.py",
    "patch_conv_success.py",
    "patch_size_stub.py",
    "patch_bufreturn.py",
]


def run(name):
    path = os.path.join(ROOT, "tools", name)
    if not os.path.exists(path):
        print("FEHLT:", name)
        return
    res = subprocess.run([sys.executable, path], cwd=ROOT,
                         capture_output=True, text=True)
    print("== %s (rc=%d)" % (name, res.returncode))
    for line in (res.stdout or "").strip().splitlines():
        print("    " + line)
    if res.returncode != 0:
        print(res.stderr)
        raise SystemExit(1)


def main():
    for name in ORDER:
        run(name)
    if "--no-retprobe" not in sys.argv:
        run("patch_retprobe.py")


if __name__ == "__main__":
    main()
