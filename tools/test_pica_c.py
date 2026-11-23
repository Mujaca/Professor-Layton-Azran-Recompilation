"""Baut und startet tests/pica_selftest.c (PICA-Registermap).

  python tools\\test_pica_c.py

Prueft 0x200-Basis, 0x201-Formate, 0x203-Loader-Stride, 0x228/0x22F-Draw,
0x80-Textur-Enable und das Textursampling ohne Gast-Code.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'tools', 'out', 'pica_selftest.exe')


def main():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), 'zig.exe')
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    cmd = [zig, 'cc', '-std=c11', '-O1',
           '-I', os.path.join(ROOT, 'runtime'),
           os.path.join(ROOT, 'tests', 'pica_selftest.c'),
           os.path.join(ROOT, 'runtime', 'gpu.c'),
           os.path.join(ROOT, 'runtime', 'arm_cpu.c'), '-o', EXE]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print('BUILD-FEHLER:\n' + p.stderr[-3000:])
        return 1
    env = dict(os.environ)
    env['HLE_PICA_RENDER'] = '1'
    r = subprocess.run([EXE], capture_output=True, text=True, env=env)
    print(r.stdout, end='')
    if r.stderr:
        print(r.stderr, end='')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
