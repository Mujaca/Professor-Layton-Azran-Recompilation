"""Engine-Draw-Methode des Menue-Nodes aufrufen (HLE_NODE_CALL)."""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
env = dict(os.environ)
env.update({
    "HLE_LOG": "1",
    "HLE_REG_ENABLE": "1",
    "HLE_ENGINE_MENU": "1",
    "HLE_MENU_OBJ0": "1",
    "HLE_DRAW_LIST": "1",
    "HLE_MENU_RING": "1",
    "HLE_NODE_CALL": "1",
    "HLE_NODE_CALL2": "1",
    "HLE_NODE_POKE0": "14004440",
    "HLE_NODE_VT": "3C6FE4",
    "HLE_NODE_ENT": "1",
    "HLE_MOUNT_PC": "28F7BC",
    "HLE_MOUNT_LOG": "1",
    "HLE_INITDUMP": "1",
    "HLE_HOOK": "1034F4=count",
    "HLE_POOL_MENUOBJ": "1",
    "HLE_BUILDER_ALLOC": "0E900000",
    "HLE_FRAME_LOOP2": "28F7BC",
    "HLE_FRAME_ARG2": "3EC028,7D0,0,FFFFFFFF",
    "HLE_FRAME_SP": "0CF0A000",
    "HLE_GSP_ENGINE": "1",
    "HLE_GSP_TRIGGER": "1",
    "HLE_GSP_BASE": "1",
    "HLE_QUEUE_LOG": "1",
    "PORT_NO_OFFICIAL": "1",
    "PORT_SHOT_FRAMES": "120",
})
log = os.path.join(ROOT, "out", "logs", "node_call_run.txt")
p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                    "--shot", os.path.join("out", "shot_node_call.bmp")],
                   cwd=ROOT, env=env, capture_output=True, text=True,
                   errors="replace")
text = (p.stdout or "") + (p.stderr or "")
open(log, "w", encoding="utf-8", newline="\n").write(text)
print("Log:", log)
for line in text.splitlines():
    if ("menu-node" in line or "node+0x1C" in line or "gspq" in line
            or "SetCommandList" in line or "qops=" in line):
        print(line)
