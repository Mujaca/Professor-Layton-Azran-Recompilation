"""Statische Analyse der dekomprimierten code.bin (ARM11)."""
import os, re, struct, collections
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CODE = os.path.join(ROOT, "out", "exefs", "code_decomp.bin")
data = open(CODE, "rb").read()
BASE = 0x100000
print(f"code (dekomprimiert): {len(data):,} bytes  ({len(data)/2**20:.2f} MiB)")
print("erste 32 Bytes:", data[:32].hex(" "))

md = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
md.detail = True

# --- ARM-Sweep mit Skip auf Fehler ---
mnem = collections.Counter()
svcs = collections.Counter()
bl_targets = collections.Counter()
n_ins = 0
for off in range(0, len(data) - 4, 4):
    for ins in md.disasm(data[off:off + 4], BASE + off):
        n_ins += 1
        mnem[ins.mnemonic] += 1
        if ins.mnemonic == 'svc':
            svcs[ins.operands[0].imm] += 1
        elif ins.mnemonic in ('bl', 'blx') and ins.operands and ins.operands[0].type == 1:
            bl_targets[ins.operands[0].imm] += 1
        break

print(f"\nARM-Instruktionen: {n_ins:,}  (Abdeckung {n_ins*4/len(data)*100:.1f}% der Bytes)")
print("Top-20 Mnemonics:")
for m, c in mnem.most_common(20):
    print(f"  {c:>8,}  {m}")

print(f"\nSVC-Aufrufe: {sum(svcs.values()):,}  verschiedene Nummern: {len(svcs)}")
for num, c in sorted(svcs.items()):
    print(f"  svc 0x{num:02X}: {c:>7,}x")
