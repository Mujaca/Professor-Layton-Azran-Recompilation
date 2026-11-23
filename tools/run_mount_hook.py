"""Mount-Hook: nach der .fa-Registrierung fn_0011E494/fn_0012493C rufen."""
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
    "HLE_MOUNT_LOG": "1",
    "HLE_MOUNT_PC": "28F7BC",
    "HLE_MOUNT_ARC": "10002254",
    "HLE_MOUNT_ARG2": "0CFE9800",
    "HLE_MOUNT_DIRECT": "1",
    "HLE_MOUNT_SUBMIT": "1",
    "HLE_MOUNT_QDUMP": "1",
    "HLE_RETFORCE": ("26F130=1,130220=1,13C46C=1000,284AA8=0CFFA000,"
                     "1300C8=1,13C4EC=1000,130170=1,132FF8=0,"
                     "133018=0,133150=0"),
    "HLE_FA_LOG": "1",
    "HLE_GSP_TRIGGER": "1",
    "HLE_GSP_ENGINE": "1",
    "HLE_GSP_BASE": "1",
    "HLE_QUEUE_LOG": "1",
    "HLE_POKE": "3E7770=00010100",
    "HLE_HOOK": ("11E494=count,12493C=count,124AD8=count,12FD30=count,"
                 "139914=count,13F2D4=count,13F474=count,13A854=count,"
                 "12FC24=count,13E3E0=count"),
    "PORT_NO_OFFICIAL": "1",
    "PORT_SHOT_FRAMES": "180",
})
if "--real-list" not in sys.argv[1:]:
    env["HLE_RETFORCE"] += ",12FC24=1"
    env["HLE_MOUNT_LISTPATCH"] = "1"
log = os.path.join(ROOT, "out", "logs", "mount_hook_run.txt")
p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                    "--shot", os.path.join("out", "shot_mount_hook.bmp")],
                   cwd=ROOT, env=env, capture_output=True, text=True,
                   errors="replace")
text = (p.stdout or "") + (p.stderr or "")
open(log, "w", encoding="utf-8", newline="\n").write(text)
print("Log:", log)
for line in text.splitlines():
    if line.startswith("[mount]") or "gspq" in line or "qops" in line:
        print(line)
for pat in (r"\[hook-final\] count addr=0011E494.*",
            r"\[hook-final\] count addr=0012493C.*",
            r"\[hook-final\] count addr=00124AD8.*",
            r"\[hook-final\] count addr=0012FD30.*",
            r"\[hook-final\] count addr=0013F2D4.*"):
    m = re.findall(pat, text)
    print(("OK  " + m[0]) if m else ("FEHLT " + pat))
