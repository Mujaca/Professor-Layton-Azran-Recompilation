"""Entfernt die GPU-Busy-Warteschleife in fn_001B9938 (Block 0x1BA898):
der Port rendert synchron, also darf der Renderer nach der ersten Pruefung
sofort zum Finalisieren (0x1BA8A8) gehen."""
p = 'build/gen_boot.c'
s = open(p, encoding='utf-8', errors='replace').read()
key = 'L_001B9938_001BA898:'
i = s.find(key)
if i < 0:
    print('block not found')
else:
    j = s.find('\nL_001B9938_', i + len(key))
    if j < 0:
        j = i + 1200
    seg = s[i:j]
    old = 'goto L_001B9938_001BA364;'
    if old in seg:
        pass
    if 'goto L_001B9938_001BA8A8;' in seg and old not in seg:
        print('already patched')
    elif old in seg:
        seg = seg.replace(old, 'goto L_001B9938_001BA8A8;', 1)
        s = s[:i] + seg + s[j:]
        open(p, 'w', encoding='utf-8', newline='\n').write(s)
        print('patched')
    else:
        print('target goto not found in block')
