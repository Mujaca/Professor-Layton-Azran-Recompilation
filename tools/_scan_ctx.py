"""Sucht Code, der ctx+0x16C/+0x170/+0x174 liest/schreibt (Node-Liste)."""
import capstone

CODE = r'C:\Users\user\Documents\Repos\3DS\out\exefs\code_decomp.bin'
code = open(CODE, 'rb').read()
WANT = {0x16C, 0x170, 0x174}
hits = []
for mode, tag in ((capstone.CS_MODE_ARM, 'A'), (capstone.CS_MODE_THUMB, 'T')):
    md = capstone.Cs(capstone.CS_ARCH_ARM, mode)
    for ins in md.disasm(code, 0x100000):
        if ins.mnemonic not in ('ldr', 'str', 'ldrh', 'strh', 'ldrb', 'strb'):
            continue
        op = ins.op_str
        if '#0x' not in op:
            continue
        try:
            off = int(op.split('#0x')[1].split(']')[0], 16)
        except Exception:
            continue
        if off in WANT:
            hits.append((ins.address, tag, ins.mnemonic, op))
print('hits', len(hits))
for a, t, m, o in hits[:30]:
    print('%08X%s %-4s %s' % (a, t, m, o))
