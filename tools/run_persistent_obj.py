"""Persistente Menue-Draw-Liste mit echtem Menue-Objekt.

Mit HLE_POOL_MENUOBJ zeigen alle Pool-Slots auf das Menue-Objekt
0x3EE12C; der Builder baut daraus einen persistenten Eintrag:

  [port] frames=29 ... dl=1/256 e0=003EE12C
  [port] frames=59 ... dl=1/256 e0=003EE12C
  [menuobj] obj[0]=003EE12C: ... 00030010 0010470C 001047DC 001047E4

  python tools/run_persistent_obj.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    frames = "60"
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--frames" and i + 1 < len(args):
            frames = args[i + 1]
    env = dict(os.environ)
    env.update({
        "HLE_ENGINE_MENU": "1",
        "HLE_MENU_OBJ0": "1",
        "HLE_DRAW_LIST": "1",
        "HLE_POOL_MENUOBJ": "1",
        "HLE_REG_ENABLE": "1",
        "HLE_BUILDER_ALLOC": "0E900000",
        "HLE_FRAME_LOOP2": "28F7BC",
        "HLE_FRAME_ARG2": "3EC028,7D0,0,FFFFFFFF",
        "HLE_FRAME_SP": "0CF0A000",
        "HLE_MENU_DUMP": "1",
        "HLE_DUMP_AT": "29",
        "PORT_NO_OFFICIAL": "1",
        "PORT_SHOT_FRAMES": frames,
    })
    log = os.path.join(ROOT, "out", "logs", "persistent_obj_run.txt")
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", os.path.join("out", "shot_persistent_obj.bmp")],
                       cwd=ROOT, env=env, capture_output=True, text=True)
    text = (p.stdout or "") + (p.stderr or "")
    open(log, "w", encoding="utf-8", newline="\n").write(text)
    print("Log:", log)
    for pat in (r"\[port\] frames=29 .*dl=1/256 e0=003EE12C.*",
                r"\[port\] frames=59 .*dl=1/256 e0=003EE12C.*",
                r"\[menuobj\] obj\[0\]=003EE12C.*"):
        m = re.findall(pat, text)
        print(("OK  " if m else "FEHLT ") + (m[0] if m else pat))
    return 0


if __name__ == "__main__":
    sys.exit(main())
