"""Baut und startet tests/dsp_hle_selftest.c (DSP-HLE-Frame-Verarbeitung).

  python tests/dsp_hle_test.py

Erwartungen sind aus Azahar audio_core/hle/{hle,shared_memory,source}.cpp
abgeleitet (Frame-Counter-Regionen, Dirty-Bits, Status-Rueckschreiben,
u32_dsp-Mittelendian, Sleep-Reply nach zwei Frames).
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'tools', 'out', 'dsp_hle_selftest.exe')


def main():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), 'zig.exe')
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    cmd = [zig, 'cc', '-std=c11', '-O1',
           '-I', os.path.join(ROOT, 'runtime', 'audio'),
           os.path.join(ROOT, 'runtime', 'audio', 'audio_codec.c'),
           os.path.join(ROOT, 'runtime', 'audio', 'dsp_hle.c'),
           os.path.join(ROOT, 'runtime', 'audio', 'host_output.c'),
           os.path.join(ROOT, 'tests', 'dsp_hle_selftest.c'),
           '-lwinmm', '-o', EXE]
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
