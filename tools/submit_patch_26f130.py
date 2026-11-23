"""Paket 6 (Ziel C): minimaler Patch-Generator fuer das 0x26F130-Gate.

BEFUND (gemessen: tools/out/submit_natural_args.txt)
====================================================
fn_0012493C ruft 0x26F130(sp+0x10, 0) und bricht bei r0 == 0 an 0x12497C
nach 0x124C2C ab (mov r0,#0; pop -> Rueckgabe 0).

0x26F130 baut intern (0x27A484) aus  base = container+0x18  und  name
einen Pfad-String und ruft 0x2808A8. Dieses verwirft den Pfad, wenn
  (a) [pfad] == 0            (0x2808C4/0x2808C8, Leer-Check), oder
  (b) CFile-Manager 0x268C80 == 0 (Pool 0x40F048, Count @+0x26 = 0).
Messung: world+0x18 und world+0x35C sind im Port 0; der Pool-Count wird
nie geschrieben (0x11ACBC laeuft nicht).

PATCH (eine Zuweisung, nur dieser eine Aufrufer)
================================================
In build/gen_boot.c, fn_0012493C, direkt nach `fn_0026F130(c);`:
    c->r[0] = 1u;
Damit ist der Ressourcen-Vorcheck erfuellt, ohne HLE_RETFORCE=26F130=1
und ohne 0x26F130 fuer andere Aufrufer (Asset-Registry-Pfade) zu
verbiegen. Semantisch identisch zu arm_retforce (runtime/arm_trace.c:
817-877: r0 setzen, Callee ueberspringen). 0x130220/0x1300C8/0x130170
bleiben unveraendert.

WICHTIG: port.exe fuehrt build/gen_boot.c aus; out/exefs/code_decomp.bin
ist nur das Speicher-Image (tests/port.c:330). Deshalb wird hier
gen_boot.c gepatcht (danach `python tools\\build_port.py`).

  python tools\\submit_patch_26f130.py            # Dry-Run
  python tools\\submit_patch_26f130.py --apply    # patcht build/gen_boot.c
  python tools\\submit_patch_26f130.py --restore  # entfernt die Zeile
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "build", "gen_boot.c")
MARK = "c->r[0] = 1u;   /* submit_patch_26f130: Gate 0x26F130 */"
CALL = ("  { uint32_t sp0 = c->r[13];\n"
        "    c->r[14] = 0x00124978u;\n"
        "    fn_0026F130(c);\n"
        "    arm_restore_sp(c, sp0); }\n")
PATCHED = CALL + "  " + MARK + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--restore", action="store_true")
    args = ap.parse_args(argv)

    s = open(GEN, encoding="utf-8").read()
    have_call = CALL in s
    have_mark = MARK in s
    print("build/gen_boot.c: call=%s patch=%s" % (have_call, have_mark))

    if args.restore:
        if not have_mark:
            print("nichts zu entfernen")
            return 0
        s = s.replace(PATCHED, CALL, 1)
        open(GEN, "w", encoding="utf-8", newline="\n").write(s)
        print("restored (rebuild: python tools\\build_port.py)")
        return 0

    if have_mark:
        print("bereits gepatcht")
        return 0
    if not have_call:
        print("Aufrufstelle in fn_0012493C nicht gefunden - Abbruch")
        return 2
    if not args.apply:
        print("Dry-Run: wuerde nach fn_0026F130(c) einfuegen: " + MARK)
        return 0
    s = s.replace(CALL, PATCHED, 1)
    open(GEN, "w", encoding="utf-8", newline="\n").write(s)
    print("gepatcht (rebuild: python tools\\build_port.py)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
