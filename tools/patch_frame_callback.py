"""Diagnose-Patch: RM-Frame-Callback trotz fehlgeschlagener Vorpruefung setzen.

fn_0011B98C registriert den Frame-/RM-Handler [scene+0xF8] (0x00101750):

    0x11BA8C  bl  0x12711c        ; letzte Initialisierung
    0x11BA90  movs r5, r0
    0x11BA98  beq 0x11baa8        ; bei 0 -> Registrierung ueberspringen
    0x11BA9C  ldr r1, [r6, #0xf8] ; = 0x00101750
    0x11BAA0  mov r0, #0x400
    0x11BAA4  bl  0x27a8a0        ; [0x40B4CC] = Handler fuer Event 0x400

Messung im Port: 0x12711c liefert 0, deshalb bleibt [0x40B4CC] = 0. Der
Resource-Manager wartet dann ewig auf seine Frame-Deadline (0x4FE74FAF,
statisch) und die Hauptschleife kommt nicht weiter.

Dieser Patch ersetzt das `beq` durch ein NOP, damit der Handler registriert
wird.

Idempotent; aendert nur out/exefs/code_decomp.bin.
Danach `python tools/build_port.py` laufen lassen.
"""

import struct

PATH = 'out/exefs/code_decomp.bin'
BASE = 0x100000
ADDR = 0x0011BA98
NOP = 0xE1A00000

with open(PATH, 'rb') as f:
    data = bytearray(f.read())

off = ADDR - BASE
old = struct.unpack_from('<I', data, off)[0]
if old == NOP:
    print('bereits gepatcht: 0x%X ist NOP' % ADDR)
else:
    print('patche 0x%X: %08X -> %08X' % (ADDR, old, NOP))
    struct.pack_into('<I', data, off, NOP)
    with open(PATH, 'wb') as f:
        f.write(data)
