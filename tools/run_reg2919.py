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
    "HLE_HOOK": "2919DC=regs,1013F0=count,1014D0=count",
    "PORT_NO_OFFICIAL": "1",
    "PORT_SHOT_FRAMES": "120",
})
log = os.path.join(ROOT, "out", "logs", "reg2919_log.txt")
p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                    "--shot", os.path.join("out", "shot_reg2919.bmp")],
                   cwd=ROOT, env=env, capture_output=True, text=True,
                   errors="replace")
text = (p.stdout or "") + (p.stderr or "")
open(log, "w", encoding="utf-8", newline="\n").write(text)
print("Log:", log)
for line in text.splitlines():
    if "2919DC" in line or "hook" in line and "regs" in line:
        print(line)
