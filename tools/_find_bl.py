import struct
import capstone

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
target = 0x138D70
for i in range(0, len(d) - 4, 4):
    w = struct.unpack_from('<I', d, i)[0]
    if (w & 0x0F000000) != 0x0B000000 and (w & 0x0F000000) != 0x0F000000:
        continue
    ins = list(md.disasm(d[i:i + 4], base + i))
    if not ins:
        continue
    ins = ins[0]
    if ins.mnemonic in ('bl', 'blx') and ('0x%08x' % target) in ins.op_str.lower():
        print('%08X %s' % (base + i, ins.op_str))
