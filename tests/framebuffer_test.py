"""Baut und startet tests/framebuffer_selftest.c (Bildausgabe).

  python tests/framebuffer_test.py

Prueft mit synthetischen Framebuffern (kein Spielcode, keine Referenzbilder):
Spaltenlayout addr + x*stride + (rows-1-y)*bpp, Stride/Fallback, alle fuenf
GSP-Pixelformate (RGBA8/RGB8/RGB565/RGB5A1/RGBA4), die Komposition beider
Screens (400x240 oben, 320x240 unten zentriert) und die GSP-FrameBufferInfo
(active_fb/address_left1/2/shown_fb) samt VBlank-Latch.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'tools', 'out', 'framebuffer_selftest.exe')


def main():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), 'zig.exe')
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    cmd = [zig, 'cc', '-std=c11', '-O1',
           '-I', os.path.join(ROOT, 'runtime', 'platform'),
           os.path.join(ROOT, 'tests', 'framebuffer_selftest.c'),
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
