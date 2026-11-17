import io

p = 'runtime/hle/hle.c'
s = io.open(p, encoding='utf-8').read()

if 'HLE_SRV_NAMES' not in s:
    old = '            if (g_log) printf("[srv] GetServiceHandle \\"%s\\" -> %u\\n", name, h);'
    if old not in s:
        # Fallback: andere Schreibweise suchen
        idx = s.find('GetServiceHandle \\"')
        print('anchor-format anders, index', idx)
    new = ('            if (g_log || getenv("HLE_SRV_NAMES"))\n'
           '                printf("[srvname] \\"%s\\" ptr=%08X blk=%08X lr=%08X -> %u\\n",\n'
           '                       name, c->r[rankin], arm_last_block, c->r[14], h);')
    if old in s:
        s = s.replace(old, new, 1)
    io.open(p, 'w', encoding='utf-8', newline='\n').write(s carefully)
print('ok')
