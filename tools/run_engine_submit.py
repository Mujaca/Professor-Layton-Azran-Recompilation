"""Diagnose-Lauf fuer den Engine-GX-Submit (0x12FD30 -> 0x139914 -> 0x13F2D4).

Der Lauf zeigt, dass die reale Submit-Funktion 0x12FD30 ihren Queue-Eintrag
in den GSP-CommandBuffer (ctx+0x58 = 0x10002800) schreibt und der
Engine-Trigger 0x13E3E0 danach qops>0 meldet.

  python tools/run_engine_submit.py
  python tools/run_engine_submit.py --frames 150 --shot out/shot_engine_submit.bmp

Erwartete Belegzeilen (siehe out/logs/engine_submit_ok.txt):

  [gspq-engine] tid=0 n=1 list=0E800000 size=1000 ops=1
  [queue] pre-trigger n=1 addr=0E800000 size=1000
  [gsp] Trigger lr=0012FE1C addr=0CFE6000 size=24 qops=1
  [hook-final] count addr=0012FD30 hits=44
  [hook-final] count addr=00139914 hits=44
  [hook-final] count addr=0013F2D4 hits=44
  [gsp] SetBufferSwap screen=0 fb=0x1F000000
  [gsp] SetBufferSwap screen=1 fb=0x1F060000
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    frames = "150"
    shot = os.path.join("out", "shot_engine_submit.bmp")
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--frames" and i + 1 < len(args):
            frames = args[i + 1]
        if a == "--shot" and i + 1 < len(args):
            shot = args[i + 1]

    env = dict(os.environ)
    env.update({
        "HLE_LOG": "1",
        "HLE_GSP_ENGINE": "1",
        "HLE_GSP_BASE": "1",
        "HLE_GSP_TRIGGER": "1",
        # Descriptor im Engine-Layout (0x14015C: [desc+4]=A,
        # [A+0x3C]=C, 0x143F78(C)=C, [C]=Anzahl) + Stack-Argument (size)
        # + PICA-Testliste. Kein HLE_RETFORCE mehr fuer 0x14015C.
        "HLE_QUEUE_LOG": "1",
        "HLE_POKE": ("0D900000=001000,0E800000=3,0E800004=F0228,"
                     "0E800008=0,0E80000C=22E,"
                     "0EA00004=0EA10000,0EA1003C=0EA20000,"
                     "0EA2002C=0,0EA20000=1"),
        "HLE_HOOK": ("12FD30=count,139914=count,14015C=count,143F78=count,"
                     "13F2D4=count,13A854=count,13E3E0=count"),
        "HLE_FRAME_LOOP2": "12FD30",
        # r0=CommandBuffer, r1=Descriptor, r2/r3=Listenpuffer:
        "HLE_FRAME_ARG2": "10002800,0EA00000,0E800000,0E800000",
        "HLE_FRAME_SP": "0D900000",
        "PORT_NO_OFFICIAL": "1",
        "PORT_SHOT_FRAMES": frames,
    })
    log = os.path.join(ROOT, "out", "logs", "engine_submit_run.txt")
    os.makedirs(os.path.dirname(log), exist_ok=True)
    p = subprocess.run([os.path.join(ROOT, "out", "port.exe"), "out\\romfs",
                        "--shot", shot], cwd=ROOT, env=env,
                       capture_output=True, text=True)
    text = (p.stdout or "") + (p.stderr or "")
    open(log, "w", encoding="utf-8", newline="\n").write(text)
    print("Log:", log)
    for pat in (r"\[gspq-engine\].*",
                r"\[queue\] pre-trigger n=1.*",
                r"\[gsp\] Trigger lr=.*qops=1",
                r"\[hook-final\] count addr=0012FD30.*",
                r"\[hook-final\] count addr=00139914.*",
                r"\[hook-final\] count addr=0014015C.*",
                r"\[hook-final\] count addr=00143F78.*",
                r"\[hook-final\] count addr=0013F2D4.*",
                r"\[gsp\] SetBufferSwap screen=[01].*"):
        m = re.findall(pat, text)
        if m:
            print("OK ", m[0])
        else:
            print("FEHLT", pat)
    return 0


if __name__ == "__main__":
    sys.exit(main())
