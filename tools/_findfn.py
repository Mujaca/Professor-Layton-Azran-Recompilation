import capstone

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)

start = 0x35E000
end = 0x361100
prev = None
cands = []
for ins in md.disasm(d[start - base:end - base], start):
    if ins.mnemonic == 'push' and 'lr' in ins.op_str:
        cands.append((ins.address, ins.op_str, prev))
    prev = (ins.address, ins.mnemonic, ins.op_str)

for a, ops, p in cands[-12:]:
    print('%08X  push %-30s prev=%s' % (a, ops, p))
