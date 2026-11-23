"""Erzwingt die vier Gates in fn_0012493C per HLE_RETFORCE (ein Build,
kein Patch) und prueft, ob der Emitter 0x12FC24 laeuft.

Gates: 0x284AA8 (2x), 0x1300C8, 0x130170 -> Rueckgabe 1.

  python tools/run_emitter_gate.py
"""
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
    "HLE_POOL_MENUOBJ": "1",
    "HLE_REG_ENABLE": "1",
    "HLE_MENU_RING": "1",
    "HLE_FA_LOG": "1",
    "HLE_FA_FORCE_POS": "465D60",
    "HLE_XQ_HOOK": "1",
    "HLE_XQ_PATH": "seq/menu_title.xq",
    "HLE_XQ_ASSET": "menu/title_b_bg.xi",
    "HLE_XQ_ASSET_POS": "CE43074",
    "HLE_XQ_ASSET_LEN": "33340",
    "HLE_RETFORCE": "268C80=0E500000,284AA8=1,1300C8=1,130170=1",
    "HLE_POKE": "0E500000=0E501000,0E501010=0CFE1200,0E500010=004C1448",
    "HLE_FRAME_LOOP2": "28F7BC",
    "HLE_FRAME_ARG2": "3EC028,7D0,0,FFFFFFFF",
    "HLE_FRAME_SP": "0CF0A000",
    "HLE_HOOK": ("12493C=count,12FC24=count,12FD30=count,139914=count,"
                 "13F2D4=count,13E3E0=count"),
    "PORT_NO_OFFICIAL": "1",
    "PORT_SHOT_FRAMES": "150",
})
log = os.path.join(ROOT, "out", "logs", "emitter_gate.txt")
p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                    "--shot", "out\\shot_emitter_gate.bmp"], cwd=ROOT, env=env,
                   capture_output=True, text=True, errors="replace")
text = (p.stdout or "") + (p.stderr or "")
open(log, "w", encoding="utf-8", newline="\n").write(text)
print("Log:", log)
for addr, name in (("0012493C", "12493C"), ("0012FC24", "12FC24"),
                   ("0012FD30", "12FD30"), ("00139914", "139914"),
                   ("0013F2D4", "13F2D4"), ("0013E3E0", "13E3E0")):
    h = re.findall(r"\[hook-final\].*addr=" + addr + r".*", text)
    print(("OK   " + h[0]) if h else ("FEHLT " + name))
for line in text.splitlines():
    if "gspq" in line or "qops=" in line:
        print(line)
