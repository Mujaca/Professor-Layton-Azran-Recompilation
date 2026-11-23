"""menu_logo.xq ueber den Engine-Reader 0x1112C4 / 0x280744 lesen.

Der Lauf ruft fn_001112C4 mit dem Pfad "seq/menu_logo.xq" und einem
synthetischen Dateiobjekt auf. hle_fa_read liest die XQ32-Bytes aus
lt6_a.fa an Offset 0x458CB8 (Start von out/fa_a/seq/menu_logo.xq).

  python tools/run_xq_read.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    frames = "120"
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--frames" and i + 1 < len(args):
            frames = args[i + 1]
    env = dict(os.environ)
    env.update({
        "HLE_FA_LOG": "1",
        "HLE_FA_FORCE_POS": "458CB8",
        "HLE_POKE": ("0E600000=2F716573,0E600004=756E656D,"
                     "0E600008=676F6C5F,0E60000C=71782E6F,0E600010=0,"
                     "0E600100=400008,"
                     "0E500000=0E501000,0E501010=0CFE1200,"
                     "0E500010=004C1448"),
        "HLE_RETFORCE": "268C80=0E500000",
        "HLE_FRAME_LOOP2": "1112C4",
        "HLE_FRAME_ARG2": "400000,0E600100,0E600000,0",
        "HLE_FRAME_SP": "0CF0A000",
        "HLE_HOOK": "1112C4=count,2808A8=count,280744=count",
        "PORT_NO_OFFICIAL": "1",
        "PORT_SHOT_FRAMES": frames,
    })
    log = os.path.join(ROOT, "out", "logs", "xq_read_run.txt")
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", os.path.join("out", "shot_xq_read.bmp")],
                       cwd=ROOT, env=env, capture_output=True, text=True)
    text = (p.stdout or "") + (p.stderr or "")
    open(log, "w", encoding="utf-8", newline="\n").write(text)
    print("Log:", log)
    for pat in (r"\[faread\] idx=0 pos=00458CB8 want=00000048 n=72",
                r"\[hook-final\] count addr=001112C4.*",
                r"\[hook-final\] count addr=002808A8.*"):
        m = re.findall(pat, text)
        print(("OK  " if m else "FEHLT ") + (m[0] if m else pat))
    return 0


if __name__ == "__main__":
    sys.exit(main())
