import struct
import capstone

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
lo, hi = 0x28FF00, 0x290140
out = []
for i in range(0, len(d) - 4, 4):
    w = struct.unpack_from('<I', d, i)[0]
    pc = base + i
    # add/sub rd, pc, #imm  (0x028F/0x024F)
    if (w & 0x0FFF0000) == 0x028F0000:
        rd = (w >> 12) & 0xF
        imm = w & 0xFFF
        rot = ((w >> 8) & 0xF) * 2
        v = imm
        if rot:
            v = ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF
        t = (pc + 8 + v) & 0xFFFFFFFF
        if lo <= t < hi:
            out.append(('add', pc, rd, t))
    # ldr rd, [pc, #imm]   (0x059F / 0x051F)
    if (w & 0x0F7F0000) == 0x051F0000:
        rd = (w >> 12) & 0xF
        imm = w & 0xFFF
        up = (w >> 23) & 1
        t = (pc + 8 + (imm if up else -imm)) & 0xFFFFFFFF
        if lo <= t < hi:
            out.append(('ldr', pc, rd, t))
print('hits', len(out))
for kind, pc, rd, t in out:
    print('%s %08X r%d -> %08X' % (kind, pc, rd, t))
