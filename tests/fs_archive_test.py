"""Baut und startet tests/fs_archive_selftest.c (fs-Archive).

  python tests/fs_archive_test.py

Prueft ohne Spielcode: SaveData-Erststart (NotFormatted), FormatSaveData/
GetFormatInfo, ExtSaveData (0x6), SharedExtSaveData (0x7), SystemSaveData
(0x8), Open/Create/Delete/Rename/OpenDirectory samt Azahar-Result-Codes und
das Host-Layout unter out/savedata/<archiv>/...
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'tools', 'out', 'fs_archive_selftest.exe')
BASE = 'tools/out/fs_archive_test/savedata'


def main():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), 'zig.exe')
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    cmd = [zig, 'cc', '-std=c11', '-O1',
           '-I', os.path.join(ROOT, 'runtime', 'kernel'),
           os.path.join(ROOT, 'runtime', 'kernel', 'fs_archive.c'),
           os.path.join(ROOT, 'tests', 'fs_archive_selftest.c'),
           '-o', EXE]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print('BUILD-FEHLER:\n' + p.stderr[-3000:])
        return 1
    shutil.rmtree(os.path.join(ROOT, 'tools', 'out', 'fs_archive_test'), ignore_errors=True)
    r = subprocess.run([EXE, BASE], cwd=ROOT, capture_output=True, text=True)
    print(r.stdout, end='')
    if r.stderr:
        print(r.stderr, end='')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
