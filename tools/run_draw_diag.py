"""Diagnose: Welche Register/Objekte sieht die echte Draw-Kette?

Ruft den Init/World/Drain-Pfad wie run_initworld auf, dumpt aber die
Register an 0x10470C, 0x288960, 0x284A98, 0x1105F8 und 0x27C08C und
testet die Container-Lookup-Keys korrekt (Key == Kandidat).

  python tools/run_draw_diag.py
"""
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
    "HLE_XFER_CLASS": "1",
    "HLE_CONT_SCAN": "1",
    "HLE_DRAINDUMP": "1",
    "HLE_RENDERER_INIT": "1",
    "HLE_RENDERER_FORCE": "1",
    "HLE_RENDERER_CTOR": "1",
    "HLE_MENU_OBJ0": "1",
    "HLE_INITDUMP": "1",
    "HLE_INITCOPY": "1",
    "HLE_DRAW_DIRECT": "1",
    "HLE_MOUNT_LOG": "1",
    "HLE_MOUNT_PC": "28F7BC",
    "HLE_WORLD_CALL": "1",
    "HLE_RING_DUMP": "1",
    "HLE_HOOK_MAX": "260",
    "HLE_XFER_VT": "1",
    "HLE_XFER_NODE": "1",
    "HLE_XFER_SPLIT": "1",
    "HLE_VT_DUMP": "1",
    "HLE_HOOK": "27A3B4=count,192D4C=count,193A94=count,12FC24=count",
    "HLE_FRAME_LOOP2": "28F7BC",
    "HLE_FRAME_ARG2": "3EC028,7D0,0,FFFFFFFF",
    "HLE_FRAME_SP": "0CF0A000",
    "HLE_LOG": "1",
    "PORT_NO_OFFICIAL": "1",
    "PORT_SHOT_FRAMES": "150",
})
for _k in ("HLE_ENGINE_2D", "HLE_MENU_RING", "HLE_ENGINE_3D", "HLE_XQ_DRAW"):
    env.pop(_k, None)
log = os.path.join(ROOT, "out", "logs", "draw_diag.txt")
text = ""
for attempt in range(8):
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", "out\\shot_draw_diag.bmp"],
                       cwd=ROOT, env=env, capture_output=True, text=True,
                       errors="replace")
    text = (p.stdout or "") + (p.stderr or "")
    if "e0=003C6FE4" in text:
        print("Lauf", attempt + 1, "erreicht Ring-Entry {vtable,obj}")
        break
open(log, "w", encoding="utf-8", newline="\n").write(text)
print("Log:", log)
for line in text.splitlines():
    if (line.startswith("[hook] ") or line.startswith("[cont] ")
            or line.startswith("[xfer]") or line.startswith("[ddraw]")
            or "qops=" in line):
        print(line)
