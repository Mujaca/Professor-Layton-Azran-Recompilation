"""Baut und startet tests/lighting_selftest.c (Fragment-Lighting gegen Azahar).

  python tests/lighting_test.py [Konfigurationen]

Teil A vergleicht p_compute_fragments (runtime/gpu/gpu.c) mit einer
getreuen C-Uebertragung von Azahar sw_lighting.cpp ComputeFragmentsColors
fuer viele zufaellige Registerzustaende (Toleranz 1 LSB). Teil B prueft
die perspektivkorrekte Interpolation von View und Quaternion (Azahar
sw_rasterizer.cpp MakeScreenCoords / get_interpolated_attribute) ueber
echte Draws mit unterschiedlichem w.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'tools', 'out', 'lighting_selftest.exe')


def main():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), 'zig.exe')
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    # gpu.c wird in die Test-Uebersetzungseinheit eingebunden (statische
    # Funktionen p_compute_fragments/p_light_lut_write direkt aufrufbar).
    cmd = [zig, 'cc', '-std=c11', '-O1',
           '-I', os.path.join(ROOT, 'runtime', 'cpu'),
           '-I', os.path.join(ROOT, 'runtime', 'gpu'),
           os.path.join(ROOT, 'tests', 'lighting_selftest.c'),
           os.path.join(ROOT, 'runtime', 'gpu', 'hw_d3d11.c'),
           os.path.join(ROOT, 'runtime', 'cpu', 'arm_cpu.c'),
           '-o', EXE,
           '-ld3d11', '-ldxgi', '-ld3dcompiler_47']
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print('BUILD-FEHLER:\n' + p.stderr[-3000:])
        return 1
    env = dict(os.environ)
    env['HLE_PICA_RENDER'] = '1'
    r = subprocess.run([EXE] + sys.argv[1:], capture_output=True, text=True, env=env)
    print(r.stdout, end='')
    if r.stderr:
        print(r.stderr, end='')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
