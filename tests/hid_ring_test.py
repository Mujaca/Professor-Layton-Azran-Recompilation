"""Baut und startet tests/hid_ring_test.c (HID-Ringpuffer-Verhalten).

  python tests/hid_ring_test.py

Prueft die reine Ringlogik aus runtime/platform/hid_ring.h ohne Gast:
Index-Umlauf aller vier Ringe (8/8/8/32), Tastenbits, Circle-Pad-
Richtungsbits (rechts/links/oben/unten/Diagonale/Schwelle 40), die
delta_additions/delta_removals gegen den Vorgaenger-Eintrag, die
Ereignis-Signalisierung als Rueckgabemaske, Touch-Koordinaten ohne Druck
(0,0) und mit Druck (inkl. Klemme) sowie die Accelerometer-Ruhelage
(0,-512,0) und deren Raw-Eintrag wie Azahar.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'tools', 'out', 'hid_ring_test.exe')


def main():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), 'zig.exe')
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    cmd = [zig, 'cc', '-std=c11', '-O1', '-Wall', '-Wextra',
           '-I', os.path.join(ROOT, 'runtime', 'platform'),
           os.path.join(ROOT, 'tests', 'hid_ring_test.c'),
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
