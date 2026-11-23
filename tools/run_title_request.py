"""Echter Request 0x1117A4 laedt jetzt seq/menu_title.xq.

Der XQ-Hook ist generisch: HLE_XQ_PATH schreibt den Pfad direkt in den
Gast-RAM (0x0E600000), HLE_FA_FORCE_POS nennt den Offset in lt6_a.fa.
menu_title.xq liegt in lt6_a.fa bei 0x465D60 (menu_logo: 0x458CB8).

  python tools/run_title_request.py
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
        "HLE_FA_FORCE_POS": "465D60",
        "HLE_XQ_HOOK": "1",
        "HLE_XQ_PATH": "seq/menu_title.xq",
        "HLE_XQ_ASSET": "menu/title_b_bg.xi",
        "HLE_XQ_ASSET_POS": "CE43074",
        "HLE_XQ_ASSET_LEN": "33340",
        "HLE_RETFORCE": "268C80=0E500000",
        "HLE_POKE": ("0E500000=0E501000,0E501010=0CFE1200,"
                     "0E500010=004C1448"),
        "HLE_FRAME_LOOP2": "28F7BC",
        "HLE_FRAME_ARG2": "3EC028,7D0,0,FFFFFFFF",
        "HLE_FRAME_SP": "0CF0A000",
        "HLE_HOOK": ("1117A4=count,280744=count,1112C4=count,"
                     "12FD30=count,139914=count,13F2D4=count,13E3E0=count"),
        "PORT_NO_OFFICIAL": "1",
        "PORT_SHOT_FRAMES": frames,
    })
    log = os.path.join(ROOT, "out", "logs", "title_request_run.txt")
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", os.path.join("out", "shot_title_request.bmp")],
                       cwd=ROOT, env=env, capture_output=True, text=True,
                       errors="replace")
    text = (p.stdout or "") + (p.stderr or "")
    open(log, "w", encoding="utf-8", newline="\n").write(text)
    print("Log:", log)
    for pat in (r"\[xqhook\] 0x1117A4 -> 0x1112C4\(seq/menu_title\.xq\)",
                r"\[faread\] idx=0 pos=00465D60 want=00000048 n=72",
                r"\[faread\] idx=0 pos=00465D(?:AC|B0|B4) want=00000004 n=4",
                r"\[xqhook\] asset 0x280744\(menu/title_b_bg\.xi\)",
                r"\[faread\] idx=1 pos=0CE43074 want=[0-9A-F]+ n=[0-9]+",
                r"\[xqhook\] asset bytes=[0-9]+ magic=494D4743",
                r"\[engine2d\] title_b_bg aus FA-Read dekodiert \(320x240\)",                r"\[hook-final\] count addr=001117A4.*"):
        m = re.findall(pat, text)
        print(("OK  " if m else "FEHLT ") + (m[0] if m else pat))
    return 0


if __name__ == "__main__":
    sys.exit(main())
