"""Echter Menue-Request (0x1117A4) loest den menu_logo.xq-Load aus.

Der Hook an 0x1117A4 ruft einmal fn_001112C4 mit dem Pfad
"seq/menu_logo.xq" auf. hle_fa_read liest die XQ32-Datei aus lt6_a.fa
(Offset 0x458CB8), inkl. 0x1000-Byte-Chunks.

  python tools/run_xq_request.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    frames = "150"
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--frames" and i + 1 < len(args):
            frames = args[i + 1]
    env = dict(os.environ)
    env.update({
        "HLE_ENGINE_MENU": "1",
        "HLE_MENU_OBJ0": "1",
        "HLE_DRAW_LIST": "1",
        "HLE_REG_ENABLE": "1",
        "HLE_FA_LOG": "1",
        "HLE_FA_FORCE_POS": "458CB8",
        "HLE_XQ_HOOK": "1",
        "HLE_RETFORCE": "268C80=0E500000",
        "HLE_POKE": ("0E600000=2F716573,0E600004=756E656D,"
                     "0E600008=676F6C5F,0E60000C=71782E6F,0E600010=0,"
                     "0E600100=400008,"
                     "0E500000=0E501000,0E501010=0CFE1200,"
                     "0E500010=004C1448"),
        "HLE_FRAME_LOOP2": "28F7BC",
        "HLE_FRAME_ARG2": "3EC028,7D0,0,FFFFFFFF",
        "HLE_FRAME_SP": "0CF0A000",
        "HLE_HOOK": "1117A4=count,280744=count,1112C4=count",
        "PORT_NO_OFFICIAL": "1",
        "PORT_SHOT_FRAMES": frames,
    })
    log = os.path.join(ROOT, "out", "logs", "xq_request_run.txt")
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", os.path.join("out", "shot_xq_request.bmp")],
                       cwd=ROOT, env=env, capture_output=True, text=True)
    text = (p.stdout or "") + (p.stderr or "")
    open(log, "w", encoding="utf-8", newline="\n").write(text)
    print("Log:", log)
    for pat in (r"\[xqhook\] 0x1117A4 .*",
                r"\[faread\] idx=0 pos=00458CB8 want=00000048 n=72",
                r"\[faread\] idx=0 pos=0045[0-9A-F]+ want=00001000 n=4096",
                r"\[hook-final\] count addr=001117A4.*"):
        m = re.findall(pat, text)
        print(("OK  " if m else "FEHLT ") + (m[0] if m else pat))
    return 0


if __name__ == "__main__":
    sys.exit(main())
