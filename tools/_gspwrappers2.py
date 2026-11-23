import struct
import capstone

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
for i in range(0, len(d) - 4, 4):
    w = struct.unpack_from('<I', d, i)[0]
    if (w & 0x0F7F0000) != 0x051F0000:
        continue
    imm = w & 0xFFF
    up = (w >> 23) & 1
    t = (base + i + 8 + (imm if up else -imm)) & 0xFFFFFFFF
    if not (0x100000 <= t < 0x400000):
        continue
    v = struct.unpack_from('<I', d, t - base)[0]
    cmd = v >> 16
    if cmd == 0x000C:
        print('%08X -> %08X val=%08X normal=%d' % (base + i, t, v, (v >> 6) & 0x3F))
