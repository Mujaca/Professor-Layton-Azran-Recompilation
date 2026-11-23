import struct

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000
lo, hi = 0x0028FF90, 0x00290050

movw = {}
movt = {}
for i in range(0, len(d) - 4, 4):
    w = struct.unpack_from('<I', d, i)[0]
    if (w & 0x0FF00000) == 0x03000000:
        rd = (w >> 12) & 0xF
        imm = (((w >> 16) & 0xF) << 12) | (w & 0xFFF)
        movw.setdefault(rd, []).append((base + i, imm))
    elif (w & 0x0FF00000) == 0x03400000:
        rd = (w >> 12) & 0xF
        imm = (((w >> 16) & 0xF) << 12) | (w & 0xFFF)
        movt.setdefault(rd, []).append((base + i, imm))

found = []
for rd, ws in movw.items():
    for a, ilo in ws:
        for b, ihi in movt.get(rd, []):
            if abs(b - a) <= 80:
                v = (ihi << 16) | ilo
                if lo <= v <= hi:
                    found.append((a, b, rd, v))
print('candidates', len(found))
for a, b, rd, v in found[:20]:
    print('%08X %08X r%d -> %08X' % (a, b, rd, v))
