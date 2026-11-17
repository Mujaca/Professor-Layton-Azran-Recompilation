import io

p = 'runtime/hle/hle.c'
s = io.open(p, encoding='utf-8').read()

old = 'static const uint32_t arbs[] = { 0x090001D4u, 0x0042BD54u, 0x0042B864u };'
new = ('static const uint32_t arbs[] = { 0x090001D4u, 0x0042BD54u, 0x0042B864u,\n'
       '                                          0x0040C628u, 0x0040C5B4u, 0x0040C5A8u };')
if old not in s:
    raise SystemExit('anchor arbs fehlt')
s = s.replace(old, new, 1687)
io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
