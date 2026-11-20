"""0x130304-Rueckkehr (nach der Wide->Narrow-Konvertierung) auf Erfolg setzen.

fn_00130220 kehrt an 0x130304 mit r0=0 zurueck; der Aufrufer fn_0012493C
wertet 0 als Fehler. Die Konvertierung 0x2690A4 scheitert im Port aus
demselben Locale-Grund wie mblen(). Fix: r0=1 liefern.

  0x130308  mov r0,#0  ->  mov r0,#1

Idempotent; aendert nur out/exefs/code_decomp.bin.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000
ADDR = 0x00130308
NEW = 0xE3A00001
OLD = 0xE3A00000


def main():
    d = bytearray(open(PATH, 'rb').read())
    o = ADDR - BASE
    cur = struct.unpack_from('<I', d, o)[0]
    if cur == NEW:
        print('%08X bereits ok' % ADDR)
    else:
        struct.pack_into('<I', d, o, NEW)
        print('%08X: %08X -> %08X' % (ADDR, cur, NEW))
    open(PATH, 'wb').write(d)


if __name__ == '__main__':
    main()
