"""Baut und startet tests/y2r_selftest.c (Y2R-Konverter).

  python tests/y2r_test.py

Synthetische Bilder (kein Spielcode): YUV422/YUV420/YUYV-Eingabe gegen eine
unabhaengige Referenzschleife, alle vier Ausgabeformate mit Alpha, Rotation
90/180/270 je Kachel, lineare und PICA-8x8-Ausgabe (Morton-Order), DMA-
Einheiten (transfer_unit/gap) und ungueltige Konfigurationen.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'tools', 'out', 'y2r_selftest.exe')


def main():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), 'zig.exe')
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    cmd = [zig, 'cc', '-std=c11', '-O1',
           '-I', os.path.join(ROOT, 'runtime', 'platform'),
           os.path.join(ROOT, 'tests', 'y2r_selftest.c'),
           '-o', EXE]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print('BUILD-FEHLER:\n' + p.stderr[-3000:])
        return 1
    r = subprocess.run([EXE], cwd=ROOT, capture_output=True, text=True)
    print(r.stdout, end='')
    if r.stderr:
        print(r.stderr, end='')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
