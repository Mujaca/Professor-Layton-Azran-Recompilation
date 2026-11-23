"""Laedt das 32-bpp-Blatt aus title_b.xa (000.xi) ueber den Engine-Reader.

title_b.xa liegt in lt6_uk.fa ab 0x1526DC0; der enthaltene IMGC-Blob
000.xi beginnt bei +0x3F0 (0x15271B0, Laenge 0x860C). Der Port dekodiert
ihn mit runtime/imgc.c (A,B,G,R) und dumpt out/engine_32bpp.bin.

  python tools/run_title_overlay.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
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
        "HLE_XQ_ASSET": "menu/title_b_sheet.xi",
        "HLE_XQ_ASSET_IDX": "2",
        "HLE_XQ_ASSET_POS": "15271B0",
        "HLE_XQ_ASSET_LEN": "860C",
        "HLE_RETFORCE": "268C80=0E500000",
        "HLE_POKE": ("0E500000=0E501000,0E501010=0CFE1200,"
                     "0E500010=004C1448"),
        "HLE_FRAME_LOOP2": "28F7BC",
        "HLE_FRAME_ARG2": "3EC028,7D0,0,FFFFFFFF",
        "HLE_FRAME_SP": "0CF0A000",
        "HLE_HOOK": "1117A4=count,280744=count,1112C4=count",
        "PORT_NO_OFFICIAL": "1",
        "PORT_SHOT_FRAMES": "150",
    })
    log = os.path.join(ROOT, "out", "logs", "title_overlay_run.txt")
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", os.path.join("out", "shot_title_overlay.bmp")],
                       cwd=ROOT, env=env, capture_output=True, text=True,
                       errors="replace")
    text = (p.stdout or "") + (p.stderr or "")
    open(log, "w", encoding="utf-8", newline="\n").write(text)
    print("Log:", log)
    for pat in (r"\[xqhook\] asset 0x280744\(menu/title_b_sheet\.xi\)",
                r"\[faread\] idx=2 pos=015271B0 want=[0-9A-F]+ n=[0-9]+",
                r"\[xqhook\] asset bytes=34316 magic=494D4743",
                r"\[engine2d\] 32bpp aus FA-Read dekodiert \(256x512\)"):
        m = re.findall(pat, text)
        print(("OK  " if m else "FEHLT ") + (m[0] if m else pat))
    return 0


if __name__ == "__main__":
    sys.exit(main())
