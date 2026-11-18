import struct

d = open('out/exefs/code_decomp.bin', 'rb').read()
base = 0x100000

for val in (0x11C384, 0x11C385, 0x1234FC, 0x1234FD):
    pat = struct.pack('<I', val)
    i = 0
    res = []
    while True:
        i = d.find(pat, i)
        if i < 0:
            break
        res.append(base + i)
        i += 4
    print(hex(val), [hex(x) for x in res[:12]])
