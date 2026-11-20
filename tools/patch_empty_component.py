"""Leere erste Pfadkomponente im RomFS-Lookup ueberspringen.

Der Pfad kommt als "/lt6/snd/uk/product.bcsar" an (nach "rom:"). Der
Komponenten-Initialisierer 0x342594 verlangt ein fuehrendes '/' und setzt
start==end==r1, d.h. die erste Komponente ist absichtlich leer. Der
Lookup-Helfer hasht dann "" und degeneriert.

Patch (Code-Cave 0x399FE8, Funktion fn_00399808 hat 0 Aufrufer):

  0x399244  ldr r0,[sp,#0x64]      ->  b 0x399FE8
  0x399FE8  (neu) ldr r0,[sp,#0xC]   ; lenChars der Komponente
  0x399FEC  (neu) cmp r0,#0
  0x399FF0  (neu) ldrne r0,[sp,#0x64]
  0x399FF4  (neu) bne 0x399248
  0x399FF8  (neu) b 0x3992E4         ; len==0 -> naechste Komponente

Damit wird "lt6" die erste gehashte Komponente.

Idempotent; aendert nur out/exefs/code_decomp.bin.
Danach `python tools/build_port.py`.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000

PATCHES = [
    (0x00399244, 0xEA000367),   # b 0x399FE8
    (0x00399FE8, 0xE59D000C),   # ldr r0,[sp,#0xC]
    (0x00399FEC, 0xE3500000),   # cmp r0,#0
    (0x00399FF0, 0x159D0064),   # ldrne r0,[sp,#0x64]
    (0x00399FF4, 0x1AFFFC93),   # bne 0x399248
    (0x00399FF8, 0xEAFFFCB9),   # b 0x3992E4
]


def main():
    d = bytearray(open(PATH, 'rb').read())
    for addr, val in PATCHES:
        off = addr - BASE
        old = struct.unpack_from('<I', d, off)[0]
        if old == val:
            print('%08X bereits ok (%08X)' % (addr, val))
            continue
        struct.pack_into('<I', d, off, val)
        print('%08X: %08X -> %08X' % (addr, old, val))
    open(PATH, 'wb').write(d)


if __name__ == '__main__':
    main()
