"""Welt-Update einmal takten und [world+0xBC4] (0x3EC9EC) beobachten."""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
env = dict(os.environ)
env.update({
    "HLE_LOG": "1",
    "HLE_ENGINE_MENU": "1",
    "HLE_MENU_OBJ0": "1",
    "HLE_DRAW_LIST": "1",
    "HLE_REG_ENABLE": "1",
    "HLE_FRAME_LOOP2": "108F70",
    "HLE_FRAME_ARG2": "3EC028,0CFE9800,0,0",
    "HLE_FRAME_SP": "0CF0A000",
    "HLE_RETFORCE": "11B98C=1,123780=1,280034=1",
    "ARM_MEMWATCHRANGE": "3EC9EC,4",
    "HLE_HOOK": "11E494=count,12493C=count,2850A8=count,284D24=count",
    "PORT_NO_OFFICIAL": "1",
    "PORT_SHOT_FRAMES": "150",
})
log = os.path.join(ROOT, "out", "logs", "world_watch.txt")
p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                    "--shot", os.path.join("out", "shot_world_watch.bmp")],
                   cwd=ROOT, env=env, capture_output=True, text=True,
                   errors="replace")
text = (p.stdout or "") + (p.stderr or "")
open(log, "w", encoding="utf-8", newline="\n").write(text)
print("Log:", log)
for line in text.splitlines():
    if "3EC9EC" in line or "mw]" in line or "hook-final" in line:
        print(line)
