"""Baut und startet tests/audio_selftest.c (DSP-HLE ADPCM/Resampling).

  python tests/audio_test.py

Die Erwartungswerte stehen als Kommentar/Handrechnung im C-Test und sind
nicht aus der Implementierung abgeleitet.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'tools', 'out', 'audio_selftest.exe')


def main():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), 'zig.exe')
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    cmd = [zig, 'cc', '-std=c11', '-O1',
           '-I', os.path.join(ROOT, 'runtime', 'audio'),
           os.path.join(ROOT, 'runtime', 'audio', 'audio_codec.c'),
           os.path.join(ROOT, 'tests', 'audio_selftest.c'),
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
