"""Patcht _merge_ldrpc_fragments: nur DIREKT angrenzende Fragmente mergen,
und aktiviert den Merge-Aufruf in generate()."""
p = r'tools\recomp\cli.py'
s = open(p, encoding='utf-8').read()

old = '''    by_addr = {f.addr: f for f in funcs}
    drop = set()
    for f in funcs:
        if f.thumb:
            continue
        for t in _ldrpc_targets(f, data):
            g = by_addr.get(t)
            if g is None or g is f or g.thumb or t in call_targets:
                continue
            if g.blocks and g.blocks[0].insns:
                first = g.blocks[0].insns[0]
                if first.id in (A.ARM_INS_PUSH, A.ARM_INS_STMDB):
                    continue
            f.blocks.extend(g.blocks)
            f.blocks.sort(key=lambda b: b.addr)
            try:
                f.calls |= g.calls
            except TypeError:
                f.calls = set(f.calls) | set(g.calls)
            drop.add(t)
    if drop:'''

new = '''    by_addr = {f.addr: f for f in funcs}
    drop = set()
    changed = True
    while changed:
        changed = False
        for f in list(funcs):
            if f.thumb or not f.blocks:
                continue
            end = max(b.addr + 4 * len(b.insns) for b in f.blocks)
            g = by_addr.get(end)
            if g is None or g is f or g.thumb or end in call_targets:
                continue
            if g.blocks and g.blocks[0].insns:
                first = g.blocks[0].insns[0]
                if first.id in (A.ARM_INS_PUSH, A.ARM_INS_STMDB):
                    continue
            f.blocks.extend(g.blocks)
            f.blocks.sort(key=lambda b: b.addr)
            try:
                f.calls |= g.calls
            except TypeError:
                f.calls = set(f.calls) | set(g.calls)
            drop.add(end)
            changed = True
    if drop:'''

if old in s:
    s = s.replace(old, new)
else:
    print('merge body not found')

if 'funcs = _merge_ldrpc_fragments(funcs, res, data)' not in s:
    s = s.replace('    generated = set(fn.addr for fn in funcs)',
                  '    funcs = _merge_ldrpc_fragments(funcs, res, data)\n'
                  '    generated = set(fn.addr for fn in funcs)')

open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('done')
