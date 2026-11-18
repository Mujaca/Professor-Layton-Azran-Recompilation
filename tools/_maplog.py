import io

p = 'runtime/hle/hle.c'
s = io.open(p, encoding='utf-8').read()

old = ('        case 0x1E: case 0x1F: case 0x20: /* MemoryBlock */\n'
       '            return 0;')

new = ('        case 0x1E: case 0x1F: case 0x20: /* MemoryBlock */\n'
       '            if (getenv("HLE_MAP_LOG"))\n'
       '                printf("[map] svc=0x%02X r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X\\n",\n'
       '                       imm, c->r[0], c->r[1], c->r[2], c->r[3], c->r[4]);\n'
       '            return 0;')

if 'HLE_MAP_LOG' not in s:
    if old not in s:
        raise SystemExit('anchor fehlt')
    s = s.replace(old, new, 1)
    io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
