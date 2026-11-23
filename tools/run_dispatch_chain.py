"""Setzt das GX-Objekt [world+0xBC4] und prueft die Dispatch-Kette
0x2850A8 -> 0x11E494 -> 0x280034 -> 0x12493C -> 0x12FC24/0x12FD30.

  python tools/run_dispatch_chain.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

env = dict(os.environ)
env.update({
    "HLE_ENGINE_MENU": "1",
    "HLE_DRAW_LIST": "1",
    "HLE_POOL_MENUOBJ": "1",
    "HLE_MENU_OBJ0": "1",
    "HLE_REG_ENABLE": "1",
    "HLE_RETFORCE": "1BC450=3EE12C,11B530=1",
    "HLE_INITDUMP": "1",
    "HLE_INITCOPY": "1",
    "HLE_MOUNT_LOG": "1",
    "HLE_MOUNT_PC": "28F7BC",
    "HLE_ENGINE_FRAME": "1",
    "HLE_ENGINE_LOG": "1",
    "HLE_ENGINE_RES": "1",
    "HLE_WORLD_CALL": "1",
    "HLE_FRAME_LOOP2": "28F7BC",
    "HLE_FRAME_ARG2": "3EC028,7D0,0,FFFFFFFF",
    "HLE_FRAME_SP": "0CF0A000",
    # GX-Objekt + Menue-Gate 0x710 setzen
    "HLE_POKE": "3ECBEC=3EE12C,3EE83C=1",
    "HLE_HOOK": ("381CE4=count,28F084=count,11E494=count,12493C=count,"
                 "12FC24=count,12FD30=count,13F2D4=count,28D424=count"),
    "HLE_LOG": "1",
    "PORT_NO_OFFICIAL": "1",
    "PORT_SHOT_FRAMES": "150",
})
for _k in ("HLE_ENGINE_2D", "HLE_MENU_RING", "HLE_NO_2D_INSTALL",
           "HLE_XFER_DRAW", "HLE_XFER_CALL"):
    env.pop(_k, None)
log = os.path.join(ROOT, "out", "logs", "dispatch_chain.txt")
text = ""
for attempt in range(10):
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", "out\\shot_dispatch_chain.bmp"],
                       cwd=ROOT, env=env, capture_output=True, text=True,
                       errors="replace")
    text = (p.stdout or "") + (p.stderr or "")
    if "[initcall]" in text:
        print("Initcall in Lauf", attempt + 1)
        break
open(log, "w", encoding="utf-8", newline="\n").write(text)
print("Log:", log)
for addr, name in (("00381CE4", "381CE4"), ("0028F084", "28F084"),
                   ("0011E494", "11E494"), ("0012493C", "12493C"),
                   ("0012FC24", "12FC24"), ("0012FD30", "12FD30"),
                   ("0013F2D4", "13F2D4"), ("0028D424", "28D424")):
    h = re.findall(r"\[hook-final\].*addr=" + addr + r".*", text)
    print(("OK   " + h[0]) if h else ("FEHLT " + name))
for line in text.splitlines():
    if "gspq" in line or "qops=" in line:
        print(line)
