"""FA-Reader 0x280744 mit echtem lt6_a.fa-Header laufen lassen.

Der Lauf zeigt:
  - fn_001112C4 -> 0x2808A8 -> 0x280744 laeuft 3x,
  - hle_fa_read liefert den ARC0-Header (72 Bytes) aus lt6_a.fa,
  - log: [faread] idx=0 pos=00000000 want=48 n=72.

  python tools/run_fa_path.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    frames = "90"
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--frames" and i + 1 < len(args):
            frames = args[i + 1]
    env = dict(os.environ)
    env.update({
        "HLE_ENGINE_MENU": "1",
        "HLE_MENU_OBJ0": "1",
        "HLE_DRAW_LIST": "1",
        "HLE_REG_ENABLE": "1",
        "HLE_FA_LOG": "1",
        # synthetisches GX-/Dateiobjekt: 0x268C80 liefert einen Slot,
        # dessen Vtable-Open 0x0CFE1200 (hle_ret_one) aufruft.
        "HLE_RETFORCE": "268C80=0E500000",
        "HLE_POKE": ("0E500000=0E501000,0E501010=0CFE1200,"
                     "0E500010=004C1448"),
        "HLE_HOOK": "2808A8=count,1112C4=count,280744=count",
        "PORT_NO_OFFICIAL": "1",
        "PORT_SHOT_FRAMES": frames,
    })
    log = os.path.join(ROOT, "out", "logs", "fa_path_run.txt")
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", os.path.join("out", "shot_fa_path.bmp")],
                       cwd=ROOT, env=env, capture_output=True, text=True)
    text = (p.stdout or "") + (p.stderr or "")
    open(log, "w", encoding="utf-8", newline="\n").write(text)
    print("Log:", log)
    for pat in (r"\[faread\] idx=0 .*n=72",
                r"\[hook-final\] count addr=002808A8.*",
                r"\[hook-final\] count addr=001112C4.*"):
        m = re.findall(pat, text)
        print(("OK  " if m else "FEHLT ") + (m[0] if m else pat))
    return 0


if __name__ == "__main__":
    sys.exit(main())
