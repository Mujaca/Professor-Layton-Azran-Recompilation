"""XQ-dekomprimierte 64x32-Textur im hle_2d_stub statt des Assets zeichnen.

Der Lauf nutzt out/menu_logo_dec.bin als Textur (0x1E200000) und skaliert
sie auf den Full-Screen. Damit stammen die Pixel erstmals aus XQ-Daten.

  python tools/run_xq_draw.py
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
        "HLE_LOG": "1",
        "HLE_ENGINE_MENU": "1",
        "HLE_MENU_OBJ0": "1",
        "HLE_DRAW_LIST": "1",
        "HLE_POOL_MENUOBJ": "1",
        "HLE_REG_ENABLE": "1",
        "HLE_BUILDER_ALLOC": "0E900000",
        "HLE_MENU_RING": "1",
        "HLE_XQ_DRAW": "1",
        "HLE_FRAME_LOOP2": "28F7BC",
        "HLE_FRAME_ARG2": "3EC028,7D0,0,FFFFFFFF",
        "HLE_FRAME_SP": "0CF0A000",
        "PORT_NO_OFFICIAL": "1",
        "PORT_SHOT_FRAMES": frames,
    })
    log = os.path.join(ROOT, "out", "logs", "xq_draw_run.txt")
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", os.path.join("out", "shot_xq_draw.bmp")],
                       cwd=ROOT, env=env, capture_output=True, text=True,
                       errors="replace")
    text = (p.stdout or "") + (p.stderr or "")
    open(log, "w", encoding="utf-8", newline="\n").write(text)
    print("Log:", log)
    for pat in (r"\[engine2d\] xq texture 64x32 @1E200000",
                r"\[engine2d\] menu-node .*node=003EE12C.*",
                r"\[gsp\] SetCommandList addr=0x0CFE6000.*lr=001ED9C8.*"):
        m = re.findall(pat, text)
        print(("OK  " if m else "FEHLT ") + (m[0] if m else pat))
    return 0


if __name__ == "__main__":
    sys.exit(main())
