"""Detailanalyse des dekomprimierten Codes: Funktionen, Thumb-Anteil, Strings."""
import os, re, collections
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REP = os.path.join(ROOT, "reports")
os.makedirs(REP, exist_ok=True)
data = open(os.path.join(ROOT, "out", "exefs", "code_decomp.bin"), "rb").read()
BASE = 0x100000

md = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
md.detail = True

bl_targets = set()
blx = 0
prolog = 0
vpush = 0
pc_rel = 0
n = 0
for off in range(0, len(data) - 4, 4):
    for ins in md.disasm(data[off:off + 4], BASE + off):
        n += 1
        m = ins.mnemonic
        if m == 'bl':
            blx += 0
            if ins.operands and ins.operands[0].type == 2:
                bl_targets.add(ins.operands[0].imm)
        elif m in ('blx', 'bx'):
            blx += 1
        elif m == 'push':
            if 'lr' in ins.op_str:
                prolog += 1
        elif m == 'vpush':
            vpush += 1
        elif m in ('ldr', 'add', 'sub') and 'pc' in ins.op_str:
            pc_rel += 1
        break

print(f"Instruktionen: {n:,}")
print(f"distinkte BL-Ziele (Funktionskandidaten): {len(bl_targets):,}")
print(f"BX/BLX: {blx:,}   VFP-Pushes: {vpush:,}   PC-relative LDR/ADD/SUB: {pc_rel:,}")
print(f"ARM-typische Prologe (push mit lr): {prolog:,}")

# Strings
strings = [m.group().decode('ascii') for m in re.finditer(rb'[\x20-\x7E]{5,}', data)]
open(os.path.join(REP, "code_strings.txt"), "w", encoding="utf-8").write("\n".join(strings))
print(f"\nStrings >=5: {len(strings):,} -> reports/code_strings.txt")

kw = ('moflex', 'mobiclip', 'romfs', '.fa', 'ctpk', 'bcsar', 'bcstm', 'gsp', 'hid:',
      'fs:', 'ndsp', 'snd', 'arc', 'script', 'puzzle', 'layton', 'ctr', 'save')
hits = collections.Counter()
samples = collections.defaultdict(list)
for s in strings:
    for k in kw:
        if k in s.lower():
            hits[k] += 1
            if len(samples[k]) < 8:
                samples[k].append(s)
print("\nSchluesselwoerter in Code-Strings:")
for k, c in hits.most_common():
    print(f"  {k:10s} {c:>4}x   " + " | ".join(samples[k][:6]))
