"""Patcht cli.generate: leere Funktionen linear disassemblieren."""
p = r'tools\recomp\cli.py'
s = open(p, encoding='utf-8').read()

if '_fill_empty_fns' not in s:
    if 'import capstone\n' not in s:
        s = s.replace('import capstone.arm as A', 'import capstone\nimport capstone.arm as A')
    helper = '''
def _fill_empty_fns(res, data):
    """Leere Funktionen mit einem linearen Block fuellen (bis Terminator)."""
    if data is None:
        return
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    n = 0
    for fn in res.functions.values():
        if fn.thumb or fn.blocks:
            continue
        o = fn.addr - 0x100000
        if o < 0 or o >= len(data):
            continue
        insns = []
        for ins in md.disasm(data[o:o + 0x100], fn.addr):
            insns.append(ins)
            if ins.mnemonic in ('bx', 'pop') and 'pc' in ins.op_str:
                break
            if len(insns) >= 64:
                break
        if insns:
            fn.blocks = [scanmod.Block(fn.addr, insns)]
            n += 1
    if n:
        print("Leere Funktionen gefuellt: %d" % n)
'''
    s = s.replace('def _all_split_ldrpc(res, data):',
                  helper + '\ndef _all_split_ldrpc(res, data):')
    s = s.replace('    if data is not None:\n        _all_split_ldrpc(res, data)',
                  '    if data is not None:\n        _fill_empty_fns(res, data)\n        _all_split_ldrpc(res, data)')
    open(p, 'w', encoding='utf-8', newline='\n').write(s)
    print('patched')
else:
    print('already')
