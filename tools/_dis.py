import sys

sys.path.insert(0, "tools")
from recomp import isa

data = open("out/exefs/code_decomp.bin", "rb").read()

a = 0x107980
while a < 0x107A60:
    ins = isa.decode(data, a)
    if ins is None:
        break
    print("  0x%08X %-9s %s" % (ins.address, ins.mnemonic, ins.op_str))
    a += ins.size
