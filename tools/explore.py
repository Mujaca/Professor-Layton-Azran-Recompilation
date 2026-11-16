"""Erkundet eine .3ds (NCSD/CCI) Datei: Container-Struktur, Krypto-Status, Markierungen."""
import sys, struct, os

def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]
def asc(b, o, n): return b[o:o+n].decode('ascii', 'replace').rstrip('\x00')

MAGICS = [b'SMDH', b'CGFX', b'IVFC', b'DVLP', b'DVLE', b'MOFLEX', b'BNRS', b'SARC', b'DARC']

def scan(f, filesize, magics=MAGICS, chunk_size=8 * 2**20):
    hits = {m: [] for m in magics}
    f.seek(0)
    base = 0
    tail = b''
    while True:
        chunk = f.read(chunk_size)
        if not chunk:
            break
        buf = tail + chunk
        for m in magics:
            start = 0
            while True:
                i = buf.find(m, start)
                if i < 0:
                    break
                hits[m].append(base - len(tail) + i)
                start = i + 1
        tail = buf[-16:]
        base += len(chunk)
    return hits

def main(path):
    size = os.path.getsize(path)
    f = open(path, 'rb')
    hdr = f.read(0x200)
    print(f"file       : {os.path.basename(path)}")
    print(f"size       : {size:,} bytes ({size/2**30:.3f} GiB, 2^n aligned: {size & (size-1) == 0})")
    print(f"NCSD magic : {asc(hdr,0x100,4)}  mediaSize=0x{u32(hdr,0x104):X}u mediaId={u64(hdr,0x108):016X}")
    for i in range(8):
        o, s = u32(hdr, 0x120 + i*8), u32(hdr, 0x124 + i*8)
        if s:
            print(f"  partition{i}: ofs=0x{o*512:X} size=0x{s*512:X} ({s*512/2**20:.1f} MiB)")
    part_off = u32(hdr, 0x120) * 512

    f.seek(part_off)
    ncch = f.read(0x200)
    print(f"\nNCCH @0x{part_off:X}: magic={asc(ncch,0x100,4)}")
    print(f"  contentSize  : 0x{u32(ncch,0x104):X}u ({u32(ncch,0x104)*512:,} bytes)")
    print(f"  partitionId  : {ncch[0x108:0x110].hex()}")
    print(f"  maker/version: 0x{u16(ncch,0x110):04X} / {u16(ncch,0x112)}")
    print(f"  programId    : {u64(ncch,0x118):016X}")
    print(f"  productCode  : '{asc(ncch,0x150,16)}'")
    print(f"  exheaderSize : 0x{u32(ncch,0x180):X}")
    print(f"  flags        : {ncch[0x184:0x18C].hex()}")
    for name, off in (("plain", 0x190), ("logo", 0x198), ("exefs", 0x1A0),
                      ("exefsHash", 0x1A8), ("romfs", 0x1AC)):
        print(f"  {name:10s}   : ofs=0x{u32(ncch,off)*512:X} size=0x{u32(ncch,off+4)*512:X}")

    f.seek(part_off + 0x200 + 0x400)
    mark = f.read(4)
    plain = mark == b'NCCH'
    print(f"\nexheader NCCH-copy @+0x600: {mark!r} -> " + ("PLAINTEXT (bereits entschluesselt)" if plain else "verschluesselt"))

    hits = scan(f, size)
    print("\nMarkierungen:")
    for m, h in hits.items():
        if h:
            print(f"  {m.decode():7s}: {len(h):5d} hits  first: " + ", ".join(hex(x) for x in h[:6]))
    f.close()

if __name__ == '__main__':
    main(sys.argv[1])
