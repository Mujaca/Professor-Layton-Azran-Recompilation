import struct

BLOB = 0x1000
d = open('out/romfs_image.bin', 'rb').read(BLOB + 0xDD80)[BLOB:]

DIR_HASH = 0x28
DIR_META = 0x4C
FILE_HASH = 0x160
FILE_META = 0xAF4


def u32(o):
    return struct.unpack_from('<I', d, o)[0]


def chain_check(label, table, count, meta, next_off, total):
    print('==', label)
    for b in range(count):
        cur = u32(table + b * 4)
        seen = set()
        steps = 0
        while cur != 0xFFFFFFFF and cur < total:
            if cur in seen:
                print('  CYCLE bucket %d at %04X (steps=%d)' % (b, cur, steps))
                break
            seen.add(cur)
            cur = u32(meta + cur + next_off)
            steps += 1
        if steps > 1000:
            print('  LONG bucket %d steps=%d' % (b, steps))


chain_check('DIR', DIR_HASH, 9, DIR_META, 0x10, 0x114)
chain_check('FILE', FILE_HASH, 613, FILE_META, 0x18, 0xD284)
