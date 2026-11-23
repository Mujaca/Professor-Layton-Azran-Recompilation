"""Patcht cli.generate: ldr-pc-Fragmentfunktionen in die umschliessende
Funktion mergen (Blocks statt eigener fn_)."""
p = r'tools\recomp\cli.py'
s = open(p, encoding='utf-8').read()

if '_merge_ldrpc_fragments' not in s:
    fn = '''
def _ldrpc_targets(fn, data):
    if fn.thumb:
        return set()
    off0 = 0x100000

    def rd(a):
        o = a - off0
        if o < 0 or o + 4 > len(data):
            return None
        return int.from_bytes(data[o:o + 4], 'little')

    out = set()
    for blk in list(fn.blocks):
        for ins in blk.insns:
            if ins.id != A.ARM_INS_LDR:
                continue
            op = ins.op_str.replace(' ', '')
            if 'pc' not in op or 'lsl#2' not in op:
                continue
            for i in range(64):
                v = rd(ins.address + 8 + i * 4)
                if v is None:
                    break
                if fn.addr <= v < fn.addr + 0x10000:
                    out.add(v)
    return out


def _merge_ldrpc_fragments(funcs, res, data):
    """Fragment-fn_ (ldr-pc-Ziele ohne Prolog) in die Elternfunktion mergen."""
    if data is None:
        return funcs
    call_targets = set()
    for f in funcs:
        for c, _ in f.calls:
            call_targets.add(c)
    by_addr = {f.addr: f for f in funcs}
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
            f.calls.extend(g.calls)
            drop.add(t)
    if drop:
        funcs = [f for f in funcs if f.addr not in drop]
        print("ldr-pc-Fragmente gemerged: %d" % len(drop))
    return funcs
'''
    s = s.replace('def generate(res, path, max_funcs=None, trace=False, data=None):',
                  fn + '\ndef generate(res, path, max_funcs=None, trace=False, data=None):')
    s = s.replace('    generated = set(fn.addr for fn in funcs)',
                  '    funcs = _merge_ldrpc_fragments(funcs, res, data)\n'
                  '    generated = set(fn.addr for fn in funcs)')
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('patched')
else:
    print('already')
