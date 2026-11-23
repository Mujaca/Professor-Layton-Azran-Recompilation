"""Kanonische Dateinamen setzen und Spam aufraeumen.

Feste Artefakte:
  out/shot.bmp          Screenshot des letzten Ketten-/Diagnoselaufs
  out/shot_engine.bmp   Screenshot des Engine-Listen-Laufs
  out/logs/run.txt      Log des letzten Ketten-/Diagnoselaufs
  out/logs/engine.txt   Log des Engine-Listen-Laufs

  python tools\\canon.py --log out\\logs\\foo.txt --shot out\\foo.bmp
  python tools\\canon.py --log out\\logs\\foo.txt --shot out\\foo.bmp --engine
  python tools\\canon.py --clean

--clean entfernt alle anderen bmp/png/logs in out; die Asset- und
RomFS-Dateien bleiben unberuehrt.
"""
import argparse
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "out")
LOGS = os.path.join(OUT, "logs")
KEEP_SHOTS = {"shot.bmp", "shot_engine.bmp"}
KEEP_LOGS = {"run.txt", "engine.txt"}


def clean():
    removed = 0
    for name in os.listdir(OUT):
        p = os.path.join(OUT, name)
        if os.path.isfile(p) and os.path.splitext(name)[1].lower() in (".bmp", ".png"):
            if name not in KEEP_SHOTS:
                os.remove(p)
                removed += 1
    if os.path.isdir(LOGS):
        for name in os.listdir(LOGS):
            if name not in KEEP_LOGS:
                os.remove(os.path.join(LOGS, name))
                removed += 1
    print("canon: %d Datei(en) entfernt" % removed)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--log")
    ap.add_argument("--shot")
    ap.add_argument("--engine", action="store_true",
                    help="Engine-Namen (engine.txt/shot_engine.bmp) statt run")
    ap.add_argument("--clean", action="store_true")
    args = ap.parse_args(argv)

    os.makedirs(LOGS, exist_ok=True)
    log_dst = os.path.join(LOGS, "engine.txt" if args.engine else "run.txt")
    shot_dst = os.path.join(OUT, "shot_engine.bmp" if args.engine else "shot.bmp")
    if args.log:
        src = os.path.join(ROOT, args.log)
        if not os.path.exists(src):
            raise SystemExit("Log fehlt: %s" % src)
        os.replace(src, log_dst)
        print("canon: Log -> %s" % log_dst)
    if args.shot:
        src = os.path.join(ROOT, args.shot)
        if not os.path.exists(src):
            raise SystemExit("Shot fehlt: %s" % src)
        os.replace(src, shot_dst)
        print("canon: Shot -> %s" % shot_dst)
    if args.clean:
        clean()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
