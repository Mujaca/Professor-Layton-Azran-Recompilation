"""Prueft die Host-Taktberechnung und die Fenstergeometrie.

  python tests/frame_clock_test.py

Baut tests/frame_clock_selftest.c mit zig cc und vergleicht die
Ganzzahl-Ergebnisse. Die Sollwerte sind hier zusaetzlich mit Fraction
nachgerechnet, damit ein Fehler in den Konstanten auffaellt:
VBlank-Rate = 268111856/4481136 Hz (59,8312 Hz), also
VBlank-Periode = 4481136/268111856 s = 16,71368 ms.
"""
import os
import subprocess
import sys
from fractions import Fraction

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "tools", "out", "frame_clock_selftest.exe")

NUM = 268111856
DEN = 4481136


def expected_offset(frame, freq):
    return (DEN * frame * freq) // NUM


def check_constants():
    rate = Fraction(NUM, DEN)
    period = Fraction(DEN, NUM)
    assert 59.82 < float(rate) < 59.84, rate
    assert abs(float(period) - 0.01671368) < 1e-8, period
    assert expected_offset(1, 10000000) == 167136
    assert expected_offset(60, 10000000) == 10028208
    assert expected_offset(NUM, 1000000000) == DEN * 1000000000
    print("ok Konstanten: %.5f Hz, %.6f ms" % (float(rate), float(period) * 1000.0))


def main():
    check_constants()
    import ziglang

    zig = os.path.join(os.path.dirname(ziglang.__file__), "zig.exe")
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    cmd = [
        zig, "cc", "-std=c11", "-O1",
        "-I", os.path.join(ROOT, "runtime", "platform"),
        os.path.join(ROOT, "tests", "frame_clock_selftest.c"),
        "-o", EXE,
        "-lwinmm",
    ]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print("BUILD-FEHLER:\n" + p.stderr[-3000:])
        return 1
    r = subprocess.run([EXE], cwd=ROOT, capture_output=True, text=True)
    print(r.stdout, end="")
    if r.stderr:
        print(r.stderr, end="")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
