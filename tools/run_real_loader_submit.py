"""Real-Loader-Lauf: wird 0x12FD30 aus 0x124ACC gerufen (ohne Frame-Loop)?"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

env = dict(os.environ)
env.update({
    "HLE_LOG": "1",
    "HLE_OBJ_FIX": "1",
    "HLE_SPAN_FIX": "1",
    "HLE_REG_FIX": "1",
    "HLE_FILE_PRELOAD": "1",
    "HLE_BIGARENA": "1",
    "HLE_REG_ENABLE": "1",
    "HLE_ENGINE_MENU": "1",
    "HLE_MENU_OBJ0": "1",
    "HLE_DRAW_LIST": "1",
    "HLE_FA_LOG": "1",
    "HLE_HOOK": ("2919DC=count,12493C=count,124AD8=count,124C30=count,"
                 "12FD30=count,139914=count,13F2D4=count,13E3E0=count"),
    "PORT_NO_OFFICIAL": "1",
    "PORT_SHOT_FRAMES": "180",
})
log = os.path.join(ROOT, "out", "logs", "real_loader_submit.txt")
p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                    "--shot", os.path.join("out", "shot_real_loader.bmp")],
                   cwd=ROOT, env=env, capture_output=True, text=True,
                   errors="replace")
text = (p.stdout or "") + (p.stderr or "")
open(log, "w", encoding="utf-8", newline="\n").write(text)
print("Log:", log)
for pat in (r"\[hook-final\] count addr=002919DC.*",
            r"\[hook-final\] count addr=0012493C.*",
            r"\[hook-final\] count addr=00124AD8.*",
            r"\[hook-final\] count addr=00124C30.*",
            r"\[hook-final\] count addr=00124ACC.*",
            r"\[hook-final\] count addr=0012FD30.*",
            r"\[hook-final\] count addr=00139914.*",
            r"\[hook-final\] count addr=0013F2D4.*",
            r"\[hook-final\] count addr=0013A854.*",
            r"\[hook-final\] count addr=0013E3E0.*",
            r"\[gspq-engine\].*",
            r"\[gsp\] Trigger .*qops=[0-9]+"):
    m = re.findall(pat, text)
    print(("OK  " + m[0]) if m else ("FEHLT " + pat))
