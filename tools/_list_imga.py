import struct


def blobs(path):
    d = open(path, 'rb').read()
    out = []
    i = 0
    while True:
        j = d.find(b'IMGC00', i)
        if j < 0:
            break
        out.append(j)
        i = j + 1
    for k, off in enumerate(out):
        end = out[k + 1] if k + 1 < len(out) else len(d)
        hdr = d[off:off + 0x48]
        s1 = struct.unpack_from('<i', hdr, 0x34)[0]
        s2 = struct.unpack_from('<i', hdr, 0x38)[0]
        sz = struct.unpack_from('<i', hdr, 0x3C)[0]
        w, h = struct.unpack_from('<hh', hdr, 0x10)
        print('%s blob %d off=%d len=%d w=%d h=%d fmt=%d bpp=%d need=%d' %
              (path, k, off, end - off, w, h,
               hdr[0x0A], hdr[0x0D], 0x48 + s2 + sz))


for p in ('out/x_title_uk/001.xi', 'out/x_title_uk/002.xi', 'out/x_title_uk/003.xi'):
    blobs(p)
