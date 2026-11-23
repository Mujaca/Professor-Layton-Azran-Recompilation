"""Generischer XPCK-Parser (Level-5) fuer Analyse."""
import struct


def parse(d, off, ln, imgc_decomp):
    b = d[off:off + ln]
    if b[:4] != b'XPCK':
        return None, []
    fc1, fc2 = b[4], b[5]
    count = ((fc2 & 0xF) << 8) | fc1
    info_off = struct.unpack_from('<H', b, 6)[0] << 2
    name_off = struct.unpack_from('<H', b, 8)[0] << 2
    data_off = struct.unpack_from('<H', b, 0xA)[0] << 2
    name_size = struct.unpack_from('<H', b, 0xE)[0] << 2
    names = imgc_decomp(b[name_off:name_off + name_size])
    ents = []
    for i in range(count):
        e = info_off + i * 12
        h, no, t1, t2, z1, z2 = struct.unpack_from('<IHHHBB', b, e)
        end = names.find(b'\0', no)
        s = names[no:end].decode('latin1') if no < len(names) else '?'
        fo = data_off + (((z1 << 16) | t1) << 2)
        sz = (z2 << 16) | t2
        ents.append(dict(name=s, off=fo, size=sz,
                         magic=b[fo:fo + 6].decode('latin1')))
    return count, ents
