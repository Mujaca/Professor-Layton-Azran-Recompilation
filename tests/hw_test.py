"""Baut und startet tests/hw_selftest.c (D3D11 gegen Software-Rasterizer).

  python tests/hw_test.py          # D3D11-Hardware-Treiber, Fallback WARP
  python tests/hw_test.py --warp   # erzwingt den WARP-Treiber

Rendert dieselben synthetischen PICA-Kommandolisten einmal mit dem
Software-Pfad (runtime/gpu/gpu.c) und einmal mit dem Hardware-Pfad
(runtime/gpu/hw_d3d11.c) und vergleicht die Gast-Framebuffer pixelweise
(Toleranz max. 1 LSB pro Kanal; Kantenpixel per Top-Left-Regel werden
gezaehlt und je Szene begrenzt). Szenen: konstante und vertexgefaerbte
Dreiecke, Viewport, Scissor, Tiefentest inkl. Tiefen-Rueckschreiben,
RGBA8 und RGB565, Texturen/TEV, Output-Merger, Stencil, Fog und
Fragment-Lighting (L0..L5, Toleranz dort 2 LSB).
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'tools', 'out', 'hw_selftest.exe')


def main():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), 'zig.exe')
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    cmd = [zig, 'cc', '-std=c11', '-O1',
           '-I', os.path.join(ROOT, 'runtime', 'cpu'),
           '-I', os.path.join(ROOT, 'runtime', 'gpu'),
           os.path.join(ROOT, 'tests', 'hw_selftest.c'),
           os.path.join(ROOT, 'runtime', 'gpu', 'gpu.c'),
           os.path.join(ROOT, 'runtime', 'gpu', 'hw_d3d11.c'),
           os.path.join(ROOT, 'runtime', 'cpu', 'arm_cpu.c'),
           '-o', EXE,
           '-ld3d11', '-ldxgi', '-ld3dcompiler_47']
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print('BUILD-FEHLER:\n' + p.stderr[-3000:])
        return 1
    args = [EXE]
    if '--warp' in sys.argv:
        args.append('--warp')
    r = subprocess.run(args, capture_output=True, text=True)
    print(r.stdout, end='')
    if r.stderr:
        print(r.stderr, end='')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
