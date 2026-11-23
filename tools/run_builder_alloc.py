"""Draw-Builder-Alloc 0x288960 fuer den Menue-Request ueberschreiben.

Der Lauf zeigt eine Builder-Allokation und eine Draw-Liste mit zwei
Einträgen:
  [builderalloc] 0x288960 -> 0E900000 (lr=00280254)
  [port] frames=29 ... dl=2/256 e0=0CFF2000 ... 0..2

  python tools/run_builder_alloc.py
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
        "HLE_BUILDER_ALLOC": "0E900000",
        "HLE_FRAME_LOOP2": "28F7BC",
        "HLE_FRAME_ARG2": "3EC028,7D0,0,FFFFFFFF",
        "HLE_FRAME_SP": "0CF0A000",
        "HLE_HOOK": ("2800C8=count,28031C=count,280338=count,"
                     "288960=count,28D424=count"),
        "PORT_NO_OFFICIAL": "1",
        "PORT_SHOT_FRAMES": frames,
    })
    log = os.path.join(ROOT, "out", "logs", "builder_alloc_run.txt")
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", os.path.join("out", "shot_builder_alloc.bmp")],
                       cwd=ROOT, env=env, capture_output=True, text=True)
    text = (p.stdout or "") + (p.stderr or "")
    open(log, "w", encoding="utf-8", newline="\n").write(text)
    print("Log:", log)
    for pat in (r"\[builderalloc\] 0x288960 .*",
                r"\[port\] frames=\d+ .*dl=2/256.*",
                r"\[hook-final\] count addr=002800C8.*",
                r"\[hook-final\] count addr=00280338.*"):
        m = re.findall(pat, text)
        print(("OK  " if m else "FEHLT ") + (m[0] if m else pat))
    return 0


if __name__ == "__main__":
    sys.exit(main())
