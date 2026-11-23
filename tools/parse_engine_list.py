"""out/engine_list.bin (0x0E800000) strukturell zerlegen.

Zeigt intrusive Listenkoepfe (next==prev==self) und Vtable-Kandidaten
(Zeiger in den Codebereich 0x00100000..0x00500000).
"""
import struct

BASE = 0x0E800000
d = open(r'C:\Users\user\Documents\Repos\3DS\out\engine_list.bin', 'rb').read()
words = list(struct.unpack_from('<%dI' % (len(d) // 4), d, 0))
print('len', len(d), 'words', len(words))
print('--- leere Listenkoepfe (next==prev==self):')
for i in range(len(words) - 1):
    a = BASE + i * 4
    if words[i] == a and words[i + 1] == a:
        print('  head @ %08X (off 0x%X)' % (a, i * 4))
print('--- Vtable-/Code-Zeiger:')
for i, v in enumerate(words):
    if 0x00100000 <= v < 0x00500000:
        print('  [%08X] = %08X' % (BASE + i * 4, v))
print('--- Bereich 0x0E800060..0x0E8000C0:')
for i in range(0x60 // 4, 0xC0 // 4, 4):
    a = BASE + i * 4
    print('  %08X: %08X %08X %08X %08X' % (
        a, words[i], words[i + 1], words[i + 2], words[i + 3]))
