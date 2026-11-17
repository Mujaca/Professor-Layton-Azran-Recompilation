import sys

sys.path.insert(0, "tools")
from recomp import isa

data = open("out/exefs/code_decomp.bin", "rb").read()
start = int(sys.argv[1], 16)
count = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x50

a = start
end = start + count
while a < end:
    ins = isa.decode(data, a)
    if ins is None:
        break
    print("  0x%08X %-9s %s" % (ins.address, ins.mnemonic, ins.op_str))
    a += ins.size
