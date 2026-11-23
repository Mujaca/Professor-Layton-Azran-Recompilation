"""Menue-Init + Draw-Builder ohne Stall (0x100B94) laufen lassen.

Der Lauf zeigt:
  - Asset-Registrierung (0x2919DC) 3x (lt6_a/b/uk),
  - Menue-Seiten-Setup 0x28F7BC und Draw-Builder 0x2800C8 laufen,
  - menu_logo-Zweig 0x28FCE8 wird erreicht,
  - die Draw-Liste bekommt transient einen Eintrag (dl=1/256).

  python tools/run_menu_flow.py

Beleg-Log: out/logs/menu_flow_run.txt
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    frames = "150"
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
        "HLE_FRAME_LOOP2": "28F7BC",
        "HLE_FRAME_ARG2": "3EC028,7D0,0,FFFFFFFF",
        "HLE_FRAME_SP": "0CF0A000",
        "HLE_HOOK": ("1013F0=count,1014EC=count,28F7BC=count,28FCE8=count,"
                     "28A1BC=count,2800C8=count,2919DC=count,104E44=count,"
                     "1112C4=count,287FF8=count,280744=count"),
        "PORT_NO_OFFICIAL": "1",
        "PORT_SHOT_FRAMES": frames,
    })
    log = os.path.join(ROOT, "out", "logs", "menu_flow_run.txt")
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", os.path.join("out", "shot_menu_flow.bmp")],
                       cwd=ROOT, env=env, capture_output=True, text=True)
    text = (p.stdout or "") + (p.stderr or "")
    open(log, "w", encoding="utf-8", newline="\n").write(text)
    print("Log:", log)
    for pat in (r"\[hook-final\] count addr=001013F0.*",
                r"\[hook-final\] count addr=001014EC.*",
                r"\[hook-final\] count addr=0028F7BC.*",
                r"\[hook-final\] count addr=0028FCE8.*",
                r"\[hook-final\] count addr=002800C8.*",
                r"\[hook-final\] count addr=002919DC.*",
                r"\[hook-final\] count addr=00104E44.*",
                r"\[hook-final\] count addr=001112C4.*",
                r"\[hook-final\] count addr=00280744.*",
                r"\[port\] frames=\d+ .*"):
        m = re.findall(pat, text)
        print(("OK  " if m else "FEHLT ") + (m[0] if m else pat))
    return 0


if __name__ == "__main__":
    sys.exit(main())
