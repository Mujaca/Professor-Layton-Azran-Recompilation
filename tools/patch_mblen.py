"""mblen-Fehler (-1) in der UTF-8->Wide-Konvertierung behandeln.

fn_0013A080 konvertiert einen Narrow-String nach Wide. Die Schleife
(0x13A154) beendet nur bei mblen()==0 (NUL). Liefert mblen() -1
(EILSEQ/kein Terminator im Fenster), laeuft sie endlos rueckwaerts.

Fix: bei r4<0 genau 1 Byte konsumieren (movlt r4,#1, n=r4), damit pro
Fehlbyte ein Zeichen geschrieben wird und am NUL sauber abgebrochen wird.

  0x13A168  nop        -> movlt r4,#1
  0x13A170  mov r2,r0  -> mov r2,r4
  0x13A0D4  nop        -> movlt r4,#1
  0x13A0DC  mov r2,r0  -> mov r2,r4

Idempotent; aendert nur out/exefs/code_decomp.bin.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000

PATCHES = [
    (0x0013A168, 0xB3A04001),   # movlt r4,#1
    (0x0013A170, 0xE1A02004),   # mov r2,r4
    (0x0013A0D4, 0xB3A04001),
    (0x0013A0DC, 0xE1A02004),
]


def main():
    d = bytearray(open(PATH, 'rb').read())
    for addr, val in PATCHES:
        off = addr - BASE
        old = struct.unpack_from('<I', d, off)[0]
        if old == val:
            print('%08X bereits ok' % addr)
            continue
        struct.pack_into('<I', d, off, val)
        print('%08X: %08X -> %08X' % (addr, old, val))
    open(PATH, 'wb').write(d)


if __name__ == '__main__':
    main()
