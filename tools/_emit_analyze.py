"""0x2692E4..0x2693E8: Ausgabepfad des Node-Builders."""
import capstone

CODE = r'C:\Users\user\Documents\Repos\3DS\out\exefs\code_decomp.bin'
code = open(CODE, 'rb').read()
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)

for ins in md.disasm(code[0x2692E4 - 0x100000:0x2692E4 - 0x100000 + 65 * 4], 0x2692E4):
    print('%08X %-8s %s' % (ins.address, ins.mnemonic, ins.op_str))
