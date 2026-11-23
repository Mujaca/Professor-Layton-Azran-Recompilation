import struct
import capstone

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
for a in range(0x13E000, 0x142000, 4):
    ins = list(md.disasm(d[a - base:a - base + 4], a))
    if not ins:
        continue
    ins = ins[0]
    o = ins.op_str
    if ins.mnemonic in ('ldr', 'str', 'ldrh', 'strh', 'ldrb', 'strb') and ('#0x58]' in o or '#0x5c]' in o or '#0x60]' in o or '#0x64]' in o):
        print('%08X %-6s %s' % (a, ins.mnemonic, o))
