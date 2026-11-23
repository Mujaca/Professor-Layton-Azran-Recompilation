import re
import struct

import capstone

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)

g = open('build/gen_boot.c', encoding='utf-8', errors='replace').read()
funcs = sorted(set(int(a, 16) for a in
                   re.findall(r'\{ 0x([0-9A-F]{8})u, fn_', g)))


def owner(addr):
    best = None
    for f in funcs:
        if f <= addr:
            best = f
        else:
            break
    return best


hits = []
for i in range(0, len(d) - 4, 4):
    w = d[i:i + 4]
    ins = list(md.disasm(w, base + i))
    if not ins:
        continue
    ins = ins[0]
    if ins.mnemonic == 'strh' and ins.op_str.endswith('#0x74'):
        hits.append(ins.address)

print('strh #0x74 hits', len(hits))
owners = {}
for a in hits:
    o = owner(a)
    owners.setdefault(o, []).append(a)
for o, xs in sorted(owners.items()):
    print('%s: %s' % (hex(o) if o else None, [hex(x) for x in xs[:4]]))
