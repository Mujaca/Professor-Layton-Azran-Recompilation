"""Init-Struct kopieren und Welt-Update mit echtem Struct aufrufen."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
env = dict(os.environ)
env.update({
    "HLE_ENGINE_MENU": "1",
    "HLE_DRAW_LIST": "1",
    "HLE_POOL_MENUOBJ": "1",
    "HLE_NO_2D_INSTALL": "1",
    "HLE_DRAIN_XFER": "1",
    "HLE_XFER_DRAW": "1",
    "HLE_XFER_CALL": "1",
    "HLE_XFER_GATE": "1",
    "HLE_NODE_CTOR": "1",
    "HLE_DRAW_DIRECT": "1",
    "HLE_CONT_SCAN": "1",
    "HLE_XFER_SCRATCH": "1",
    "HLE_DRAINDUMP": "1",
    "HLE_RENDERER_INIT": "1",
    "HLE_RENDERER_FORCE": "1",
    "HLE_RENDERER_CTOR": "1",
    "HLE_MENU_OBJ0": "1",
    "HLE_INITDUMP": "1",
    "HLE_INITCOPY": "1",
    "HLE_MOUNT_LOG": "1",
    "HLE_MOUNT_PC": "28F7BC",
    "HLE_WORLD_CALL": "1",
    "HLE_RING_DUMP": "1",
    "HLE_HOOK": "277004=count,2800C8=count,28D424=count,1ED8FC=count,10470C=count,1047DC=count,288960=count",
    "HLE_FRAME_LOOP2": "28F7BC",
    "HLE_FRAME_ARG2": "3EC028,7D0,0,FFFFFFFF",
    "HLE_FRAME_SP": "0CF0A000",
    "HLE_LOG": "1",
    "PORT_NO_OFFICIAL": "1",
    "PORT_SHOT_FRAMES": "150",
})
for _k in ("HLE_ENGINE_2D", "HLE_MENU_RING", "HLE_ENGINE_3D", "HLE_XQ_DRAW"):
    env.pop(_k, None)
log = os.path.join(ROOT, "out", "logs", "initworld_run.txt")
print("2D=", env.get("HLE_ENGINE_2D"), "MENU=", env.get("HLE_ENGINE_MENU"))
p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                    "--shot", os.path.join("out", "shot_initworld.bmp")],
                   cwd=ROOT, env=env, capture_output=True, text=True,
                   errors="replace")
text = (p.stdout or "") + (p.stderr or "")
open(log, "w", encoding="utf-8", newline="\n").write(text)
print("Log:", log)
for line in text.splitlines():
    if ("initcall" in line or "[world]" in line or "gspq" in line
            or "qops=" in line):
        print(line)
