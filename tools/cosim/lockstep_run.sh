#!/bin/bash
# Speicherabbild + Blockspur am 64. Aufruf, dann Vergleich mit Unicorn
cd "$(dirname "$0")/../.."
D=${DD:-scratch/cosim}
rm -rf out/btrace.txt out/watchdump_* $D
ARM_WATCH=${W:-0x17A6A0} ARM_WATCH_DUMP="${N:-64},00100000,00500000,08000000,06000000,14000000,03000000" ARM_BTRACE_MAX=${MAX:-60000} timeout 200 ./out/port.exe --frames 900 2>/dev/null >/dev/null
mkdir -p $D && mv out/watchdump_* $D/ && cp out/btrace.txt $D/
timeout 1500 python tools/cosim/lockstep.py $D $D/btrace.txt
