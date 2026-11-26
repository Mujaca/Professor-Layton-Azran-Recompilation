"""Baut und startet tests/vs_selftest.c (Vertex-Shading parallel gegen seriell).

  python tests/vs_test.py [Faelle]

Vergleicht das deduplizierende, auf den Raster-Pool verteilte
Vertex-Shading eines Draws (p_vs_shade_draw, runtime/gpu/gpu.c) bitgleich
mit dem seriellen Pfad (p_load_attrs + p_vs_process je Draw-Position) fuer
zufaellige Vertex-Programme, Swizzles, Uniforms, Vertexdaten und
Index-Puffer.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'tools', 'out', 'vs_selftest.exe')


def main():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), 'zig.exe')
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    # gpu.c wird in die Test-Uebersetzungseinheit eingebunden (statische
    # Funktionen p_vs_shade_draw/p_vs_process direkt aufrufbar).
    cmd = [zig, 'cc', '-std=c11', '-O1',
           '-I', os.path.join(ROOT, 'runtime', 'cpu'),
           '-I', os.path.join(ROOT, 'runtime', 'gpu'),
           os.path.join(ROOT, 'tests', 'vs_selftest.c'),
           os.path.join(ROOT, 'runtime', 'gpu', 'hw_d3d11.c'),
           os.path.join(ROOT, 'runtime', 'cpu', 'arm_cpu.c'),
           '-o', EXE,
           '-ld3d11', '-ldxgi', '-ld3dcompiler_47']
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print('BUILD-FEHLER:\n' + p.stderr[-3000:])
        return 1
    r = subprocess.run([EXE] + sys.argv[1:], capture_output=True, text=True)
    print(r.stdout, end='')
    if r.stderr:
        print(r.stderr, end='')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
