import struct
import re
import capstone

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
regions = [(0x240000, 0x250000), (0x340000, 0x360000),
           (0x130000, 0x145000), (0x1B9000, 0x1BB000)]
pat = re.compile(r'#(0x800|0x200|0x1000|0x3000)\]?$')
for lo, hi in regions:
    for a in range(lo, hi, 4):
        i = a - base
        ins = list(md.disasm(d[i:i + 4], a))
        if not ins:
            continue
        ins = ins[0]
        if pat.search(ins.op_str):
            print('%08X %-6s %s' % (a, ins.mnemonic, ins.op_str))
