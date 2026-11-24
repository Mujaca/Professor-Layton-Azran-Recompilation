"""Kommandozeile des Recompiler-MVP.

  python -m recomp.cli scan --in out/exefs/code_decomp.bin
  python -m recomp.cli gen  --in out/exefs/code_decomp.bin --out build/generated.c
"""
import argparse
import json
import os
import sys

import capstone
import capstone.arm as A
from . import isa, emit
from . import scan as scanmod


# Spielspezifische Block-Hooks (technische Schuld, siehe docs/WORKAROUNDS.md).
# Der Recompiler selbst bleibt spielunabhaengig; Hooks kommen nur aus dieser Datei.
def _load_block_hooks():
    import json as _json, os as _os2
    path = _os2.path.join(_os2.path.dirname(_os2.path.dirname(_os2.path.abspath(__file__))),
                          "workarounds", "block_hooks.json")
    if not _os2.path.exists(path):
        return {}
    data = _json.load(open(path, encoding="utf-8"))
    return {int(h["addr"], 16): list(h["c"]) for h in data.get("hooks", [])}


BLOCK_HOOKS = _load_block_hooks()


def load(path):
    with open(path, "rb") as f:
        return f.read()


def instruction_lines(em, ins):
    try:
        return em.emit(ins), True
    except emit.Unsupported:
        return emit.fallback(ins), False
    except Exception:
        return emit.fallback(ins), False


def collect_stats(res, data):
    arm_funcs = 0
    thumb_funcs = 0
    blocks = 0
    arm_ins = 0
    thumb_ins = 0
    unsupported = {}
    per_func = []

    for addr in sorted(res.functions):
        fn = res.functions[addr]
        if not fn.blocks:
            continue
        if fn.thumb:
            thumb_funcs += 1
            em = emit.Emitter(addr, True)
            for blk in fn.blocks:
                for ins in blk.insns:
                    thumb_ins += 1
                    _, ok = instruction_lines(em, ins)
                    if not ok:
                        key = ins.mnemonic.split(".")[0]
                        unsupported[key] = unsupported.get(key, 0) + 1
            continue
        arm_funcs += 1
        blocks += len(fn.blocks)
        em = emit.Emitter(addr)
        f_ins = 0
        f_bad = 0
        for blk in fn.blocks:
            for ins in blk.insns:
                arm_ins += 1
                f_ins += 1
                _, ok = instruction_lines(em, ins)
                if not ok:
                    f_bad += 1
                    key = ins.mnemonic.split(".")[0]
                    unsupported[key] = unsupported.get(key, 0) + 1
        per_func.append({"addr": addr, "insns": f_ins, "unsupported": f_bad})

    total_ins = arm_ins + thumb_ins
    return {
        "arm_functions": arm_funcs,
        "thumb_functions": thumb_funcs,
        "blocks": blocks,
        "arm_instructions": arm_ins,
        "thumb_instructions": thumb_ins,
        "instructions": total_ins,
        "unsupported_total": sum(unsupported.values()),
        "unsupported_pct": round(100.0 * sum(unsupported.values()) / max(1, arm_ins + thumb_ins),  2),
        "unsupported_by_mnemonic": dict(sorted(unsupported.items(), key=lambda kv: -kv[1])),
        "visited_bytes": sum(res.visited.values()),
        "text_bytes": len(data),
        "coverage_pct": round(100.0 * sum(res.visited.values()) / len(data), 2),
        "largest_functions": sorted(per_func, key=lambda d: -d["insns"])[:15],
    }


def return_bx_in_block(blk):
    """BX-Register, das aus dem Stack geladen wurde (Rueckkehr-Idiom).

    ARM/Thumb-Epilogen laden das Rueckkehrziel oft nach rN
    (`pop {rN}; ... bx rN`, `ldm sp!, {...}; bx rN`). Das ist eine Rueckkehr
    in die aufrufende C-Funktion, kein indirekter Aufruf: `arm_call_guest`
    wuerde am Rueckkehrziel eine (ggf. falsch geseedete) Gastfunktion starten
    und deren Prolog ueberspringen. Rueckgabe-Instruktionen werden daher als
    `return` uebersetzt.
    """
    import capstone.arm as A
    out = set()
    last = {}

    def regno(r):
        if A.ARM_REG_R0 <= r <= A.ARM_REG_R12:
            return r - A.ARM_REG_R0
        if r == A.ARM_REG_SP:
            return 13
        if r == A.ARM_REG_LR:
            return 14
        if r == A.ARM_REG_PC:
            return 15
        return None

    stack_load_ids = (A.ARM_INS_POP, A.ARM_INS_LDM, A.ARM_INS_LDMIB,
                      A.ARM_INS_LDMDA, A.ARM_INS_LDMDB)
    for ins in blk.insns:
        if ins.id == A.ARM_INS_BX:
            ops = isa.split_operands(ins.op_str)
            reg = isa.reg_num(ops[0]) if ops else None
            if reg is not None and reg not in (14, 15) and last.get(reg) in ("sp", "lr"):
                out.add(ins.address)
            continue
        try:
            _, written = ins.regs_access()
        except Exception:
            written = ()
        src_lr = False
        if ins.id == A.ARM_INS_MOV:
            ops = isa.split_operands(ins.op_str)
            if len(ops) >= 2:
                src = isa.reg_num(ops[1])
                dst = isa.reg_num(ops[0])
                if src == 14:
                    src_lr = True
                if (dst == 15 and src is not None and src != 15
                        and last.get(src) == "lr"):
                    # `mov rN, lr` ... `mov pc, rN` ist eine Rueckkehr in den
                    # C-Aufrufer (PIC-Epilog), kein indirekter Gastaufruf.
                    out.add(ins.address)
        is_stack_load = ins.id in stack_load_ids
        if is_stack_load:
            if ins.id == A.ARM_INS_POP:
                is_stack_load = True
            else:
                base = isa.split_operands(ins.op_str)[0].rstrip("!") if ins.op_str else ""
                is_stack_load = isa.reg_num(base) == 13
        for r in written:
            rr = regno(r)
            if rr is None or rr in (13, 15):
                continue
            last[rr] = "lr" if src_lr else ("sp" if is_stack_load else "other")
    return out


def needs_fallthrough(ins, in_it=False):
    """True, wenn nach dieser Instruktion der naechste Block folgt."""
    import capstone
    import capstone.arm as A
    # Ein Terminator in einem Thumb-2-IT-Block ist bedingt ausgefuehrt, auch
    # wenn seine Kodierung kein Bedingungsfeld traegt.
    if in_it:
        return True
    # Bedingt ausgefuehrte Terminatoren haben immer einen Fallthrough
    if ins.cc != A.ARM_CC_AL:
        return True
    if ins.id == A.ARM_INS_B:
        return False
    if ins.id == A.ARM_INS_BX:
        return False
    if ins.id == A.ARM_INS_BLX:
        for op in ins.operands:
            if op.type == A.ARM_OP_REG:
                return False
        return True
    if ins.id == A.ARM_INS_POP:
        return "pc" not in ins.op_str
    if ins.id == A.ARM_INS_LDM:
        return "pc" not in ins.op_str
    if ins.id in (A.ARM_INS_LDR, A.ARM_INS_MOV) and ins.op_str.startswith("pc"):
        return False
    if ins.id in (A.ARM_INS_ADD, A.ARM_INS_SUB) and ins.op_str.startswith("pc"):
        return False
    return True



def _split_ldrpc_blocks(fn, data):
    """ldr pc,[pc,rN,lsl#2]-Tabellenziele als eigene Blocks anlegen."""
    if fn.thumb :
        return
    off0 = 0x100000

    def rd(a):
        o = a - off0
        if o < 0 or o + 4 > len(data):
            return None
        return int.from_bytes(data[o:o + 4], 'little')

    targets = set()
    for blk in list(fn.blocks):
        for ins in blk.insns:
            if ins.id != A.ARM_INS_LDR :
                continue
            op = ins.op_str.replace(' ', '')
            if 'pc' not in op or 'lsl#2' not in op:
                continue
            tbl = ins.address + 8
            for i in range(64):
                v = rd(tbl + i * 4)
                if v is None:
                    break
                if fn.addr <= v < fn.addr + 0x10000:
                    targets.add(v)
    for t in sorted(targets):
        if any(b.addr == t for b in fn.blocks):
            continue
        for i, b in enumerate(fn.blocks):
            if b.addr >= t:
                continue
            idx = None
            for j, x in enumerate(b.insns):
                if x.address == t:
                    idx = j
                    break
            if idx is None or idx == 0:
                continue
            nb = scanmod.Block(t, b.insns[idx:])
            b.insns = b.insns[:idx]
            fn.blocks.insert(i + 1, nb)
            break


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



def _fill_empty_fns(res, data):
    """Leere Funktionen mit einem linearen Block fuellen (bis Terminator)."""
    if data is None:
        return
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    md.detail = True
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

def _all_split_ldrpc(res, data):
    """Globaler Vor-Pass: an ldr-pc-Zielen in der Elternfunktion splitten und
    leere Duplikat-Funktionen an diesen Zielen entfernen."""
    for f in list(res.functions.values()):
        if f.thumb:
            continue
        for t in sorted(_ldrpc_targets(f, data)):
            idx = None
            bi = None
            for i, b in enumerate(f.blocks):
                for j, x in enumerate(b.insns):
                    if x.address == t:
                        idx, bi = j, i
                        break
                if idx is not None:
                    break
            if idx is not None and idx > 0:
                b = f.blocks[bi]
                nb = scanmod.Block(t, b.insns[idx:])
                b.insns = b.insns[:idx]
                f.blocks.insert(bi + 1, nb)
            g = res.functions.get(t)
            if g is not None and g is not f and not g.blocks and idx is not None:
                del res.functions[t]


def _merge_ldrpc_fragments(funcs, res, data):
    """Fragment-fn_ (ldr-pc-Ziele ohne Prolog) in die Elternfunktion mergen."""
    if data is None:
        return funcs
    call_targets = set()
    for f in funcs:
        for c, _ in f.calls:
            call_targets.add(c)
    by_addr = {f.addr: f for f in funcs}
    owners = {}
    for f in funcs:
        for t in _ldrpc_targets(f, data):
            owners.setdefault(t, set()).add(f.addr)
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
            if not owners.get(end, set()) <= {f.addr}:
                continue
            if g.blocks and g.blocks[0].insns:
                first = g.blocks[0].insns[0]
                if first.id in (A.ARM_INS_PUSH, A.ARM_INS_STMDB):
                    continue
            f.blocks.extend(g.blocks)
            f.blocks.sort(key=lambda b: b.addr)
            seen = set()
            uniq = []
            for b in f.blocks:
                if b.addr in seen:
                    continue
                seen.add(b.addr)
                uniq.append(b)
            f.blocks = uniq
            try:
                f.calls |= g.calls
            except TypeError:
                f.calls = set(f.calls) | set(g.calls)
            drop.add(end)
            changed = True
    if drop:
        funcs = [f for f in funcs if f.addr not in drop]
        print("ldr-pc-Fragmente gemerged: %d" % len(drop))
    return funcs

def has_pc_relative_lr(fn):
    """True, wenn die Funktion LR aus PC berechnet (`add lr, pc, #imm`).

    Damit ruft der Code Tabellen-Handler wie Unterprogramme auf; deren
    `mov pc, lr` kehrt mitten in diese Funktion zurueck. Der Emitter muss
    dieses Rueckkehrziel nach `arm_call_guest` als lokalen Block fortsetzen,
    sonst bricht der Gastpfad ab (SP-Drift, arm_restore_sp korrigiert).
    """
    for blk in fn.blocks:
        for ins in blk.insns:
            v = scanmod.pc_relative_const(ins, ins.address, fn.thumb)
            if v is None:
                continue
            ops = isa.split_operands(ins.op_str)
            if ops and isa.reg_num(ops[0]) == 14:
                return True
    return False


def has_indirect_pc(fn):
    """True, wenn die Funktion PC aus einem Register/Speicher setzt.

    `mov pc, lr` ist eine Rueckkehr und braucht keine dynamische Tabelle;
    alle anderen PC-Schreibweisen (ldr pc, add pc, mov pc, rN) koennen auf
    beliebige Instruktionen der Funktion zeigen.
    """
    import capstone.arm as A
    for blk in fn.blocks:
        for ins in blk.insns:
            if ins.id in (A.ARM_INS_ADD, A.ARM_INS_SUB,
                          A.ARM_INS_LDR, A.ARM_INS_MOV,
                          A.ARM_INS_TBB, A.ARM_INS_TBH):
                ops = isa.split_operands(ins.op_str)
                if not ops or not isa.is_pc(ops[0]):
                    continue
                if ins.id == A.ARM_INS_MOV and len(ops) > 1 and \
                        isa.reg_num(ops[1]) == 14:
                    continue
                return True
    return False


def generate(res, path, max_funcs=None, trace=False, data=None):
    u32 = lambda v: v & 0xFFFFFFFF
    needed = set()
    for fn in res.functions.values():
        for call, _ in fn.calls:
            needed.add(call)
    if data is not None:
        for _a in needed:
            if _a not in res.functions:
                res.functions[_a] = scanmod.Function(_a)
    all_addrs = sorted(set(res.functions) | needed)
    if data is not None:
        _fill_empty_fns(res, data)
        _all_split_ldrpc(res, data)
        for _f in res.functions.values():
            _seen = set()
            _u = []
            for _b in sorted(_f.blocks, key=lambda b: b.addr):
                if _b.addr in _seen:
                    continue
                _seen.add(_b.addr)
                _u.append(_b)
            _f.blocks = _u
    funcs = [res.functions[a] for a in all_addrs if a in res.functions and res.functions[a].blocks]
    if max_funcs:
        funcs = funcs[:max_funcs]
    generated = set(fn.addr for fn in funcs)

    out = ["/* generiert vom Recompiler-MVP */", '#include "arm_cpu.h"',
           '#include <stdio.h>']
    out.append('#include "arm_trace.h"')
    out.append("")
    for a in all_addrs:
        out.append("void fn_%08X(ArmCpu *c);" % u32(a))
    out.append("")
    out.append("typedef struct { uint32_t addr; void (*fn)(ArmCpu *c); } ArmEntry;")
    out.append("static void arm_call_guest(ArmCpu *c, uint32_t addr);")
    out.append("static const ArmEntry g_entries[] = {")
    for a in all_addrs:
        fn = res.functions.get(a)
        key = u32(a) | (1 if (fn is not None and fn.thumb) else 0)
        out.append("    { 0x%08Xu, fn_%08X }," % (key, u32(a)))
    out.append("};")
    # Hash-Tabelle fuer arm_call_guest: offene Adressierung (linear probing)
    # ueber die Eintragsindizes. Der Dispatcher laeuft im Hot Path (jeder
    # indirekte Aufruf/Tail-Call); zwei Binaersuchen ueber 13k Eintraege
    # kosteten dort ~14 gestreute Speicherzugriffe, jetzt 1-2.
    n_entries = len(all_addrs)
    nb = 1
    while nb < n_entries * 4 and nb < (1 << 16):
        nb <<= 1
    nbits = nb.bit_length() - 1
    bucket = [0] * nb
    for idx, a in enumerate(all_addrs):
        fn = res.functions.get(a)
        key = u32(a) | (1 if (fn is not None and fn.thumb) else 0)
        h = ((key * 2654435761) & 0xFFFFFFFF) >> (32 - nbits)
        for probe in range(nb):
            slot = (h + probe) & (nb - 1)
            if bucket[slot] == 0:
                bucket[slot] = idx + 1
                break
    out.append("static int g_miss_printed = 0, g_miss_logged = 0;")
    out.append("#define ARM_CALL_BUCKETS %du" % nb)
    out.append("#define ARM_CALL_SHIFT %du" % (32 - nbits))
    out.append("#define ARM_CALL_MASK %du" % (nb - 1))
    out.append("static const uint16_t g_call_bucket[ARM_CALL_BUCKETS] = {")
    for i in range(0, nb, 16):
        out.append("    " + " ".join("%uu," % v for v in bucket[i:i + 16]))
    out.append("};")
    out.append("static const ArmEntry *arm_call_lookup(uint32_t addr) {")
    out.append("    uint32_t h = (addr * 2654435761u) >> ARM_CALL_SHIFT;")
    out.append("    for (uint32_t i = 0; i < 24u; i++) {")
    out.append("        uint32_t k = g_call_bucket[(h + i) & ARM_CALL_MASK];")
    out.append("        if (k == 0) return 0;")
    out.append("        if (g_entries[k - 1].addr == addr) return &g_entries[k - 1];")
    out.append("    }")
    out.append("    return 0;")
    out.append("}")
    out.append("static void arm_call_guest(ArmCpu *c, uint32_t addr) {")
    out.append("    const ArmEntry *e = arm_call_lookup(addr);")
    out.append("    if (e != 0) { e->fn(c); return; }")
    out.append("    e = arm_call_lookup(addr ^ 1u);   /* Thumb-Bit */")
    out.append("    if (e != 0) { e->fn(c); return; }")
    out.append("    { unsigned i; for (i = 0; i < sizeof(g_entries)/sizeof(g_entries[0]); i++) {")
    out.append("        if (g_entries[i].addr == addr) { g_entries[i].fn(c); return; } } }")
    out.append("    if (g_miss_printed < 200) { g_miss_printed++;")
    out.append("      fprintf(stderr, \"arm: unbekanntes call-Ziel 0x%08X (von Block 0x%08X)\\n\",")
    out.append("              addr, arm_last_block); }")
    out.append("    if (g_miss_logged < 8000) { g_miss_logged++;")
    out.append("      FILE *mf = fopen(\"out/missing_calls.txt\", \"a\");")
    out.append("      if (mf) { fprintf(mf, \"%08X\\n\", addr); fclose(mf); } }")
    out.append("    /* unbekanntes Ziel: als No-op behandeln (Best-Effort-Modus) */")
    out.append("}")
    out.append("void arm_call_guest_pub(ArmCpu *c, uint32_t addr) { arm_call_guest(c, addr); }")
    out.append("")

    n_arm = 0
    n_thumb = 0
    for fn in funcs:
        if fn.thumb:
            n_thumb += 1
        else:
            n_arm += 1
        if data is not None:
            _split_ldrpc_blocks(fn, data)
        # Doppelte Blockadressen (aus Tabellen-Splits) zusammenfassen; zwei
        # Labels an derselben Adresse waeren sonst ein C-Fehler.
        uniq = {}
        for b in fn.blocks:
            if b.addr not in uniq or len(b.insns) > len(uniq[b.addr].insns):
                uniq[b.addr] = b
        fn.blocks = [uniq[a] for a in sorted(uniq)]
        em = emit.Emitter(fn.addr, fn.thumb)
        em.block_labels = set(b.addr for b in fn.blocks)
        em.cont_after_call = has_pc_relative_lr(fn)
        if has_indirect_pc(fn):
            em.pc_targets = set(i.address for b in fn.blocks for i in b.insns)
        out.append("void fn_%08X(ArmCpu *c) {" % u32(fn.addr))
        _entry = fn.addr if any(b.addr == fn.addr for b in fn.blocks) else fn.blocks[0].addr
        out.append("  goto %s;" % em.label(_entry))
        labeled = set()
        for blk in fn.blocks:
            if blk.addr in labeled:
                continue
            out.append("%s:" % em.label(blk.addr))
            labeled.add(blk.addr)
            em.ret_bx = return_bx_in_block(blk)
            em.it_cond = emit.it_conditions(blk.insns) if fn.thumb else {}
            for ln in BLOCK_HOOKS.get(u32(blk.addr), ()):
                out.append("  " + ln)
            if trace:
                out.append("  arm_trace_block(c, 0x%08Xu, %du);" % (u32(blk.addr), 1 if fn.thumb else 0))
            for ins in blk.insns:
                if (em.pc_targets is not None
                        and ins.address not in labeled):
                    out.append("%s:" % em.label(ins.address))
                    labeled.add(ins.address)
                lines, _ = instruction_lines(em, ins)
                for ln in lines:
                    out.append("  " + ln)
            if blk.insns and needs_fallthrough(
                    blk.insns[-1], fn.thumb and id(blk.insns[-1]) in em.it_cond):
                nxt = blk.insns[-1].address + blk.insns[-1].size
                known = set(b.addr for b in fn.blocks)
                if nxt in known:
                    out.append("  goto %s;" % em.label(nxt))
                else:
                    out.append("  return;")
        out.append("  return;")
        out.append("}")
        out.append("")

    # Stubs fuer alles, was aufgerufen wird, aber nicht generiert wurde
    for a in all_addrs:
        if a in generated:
            continue
        fn = res.functions.get(a)
        reason = "thumb" if (fn is not None and fn.thumb) else ("empty" if fn else "missing")
        out.append("void fn_%08X(ArmCpu *c) {" % u32(a))
        out.append('  arm_unimplemented(c, 0x%08Xu, 0, "%s");' % (u32(a), reason))
        out.append("}")
    out.append("")

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    return n_arm, n_thumb


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_scan = sub.add_parser("scan")
    p_scan.add_argument("--in", dest="inp", required=True)
    p_scan.add_argument("--json")
    p_scan.add_argument("--prologues", action="store_true")
    p_scan.add_argument("--trace", dest="trace_path")
    p_scan.add_argument("--seed-pointers", action="store_true")
    p_scan.add_argument("--seed-file")

    p_gen = sub.add_parser("gen")
    p_gen.add_argument("--in", dest="inp", required=True)
    p_gen.add_argument("--out", required=True)
    p_gen.add_argument("--prologues", action="store_true")
    p_gen.add_argument("--max-funcs", type=int)
    p_gen.add_argument("--trace", dest="trace_flag", action="store_true")
    p_gen.add_argument("--seed-pointers", action="store_true")
    p_gen.add_argument("--seed-file")

    args = ap.parse_args(argv)
    data = load(args.inp)
    entries = [(isa.ARM_BASE, "entry", False)]
    seed_file = getattr(args, "seed_file", None)
    if seed_file:
        import os as _os
        if _os.path.exists(seed_file):
            seen = set()
            with open(seed_file, encoding="utf-8") as sf:
                for line in sf:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        addr = int(line, 16)
                    except ValueError:
                        continue
                    if addr in seen:
                        continue
                    seen.add(addr)
                    entries.append((addr & ~1, "seedfile", bool(addr & 1)))
            # Thumb-Seeds vor ARM-Seeds derselben Adresse einsortieren
            head = entries[:1]
            tail = sorted(entries[1:], key=lambda e: (e[0], 0 if e[2] else 1))
            entries = head + tail
    if getattr(args, "trace_path", None):
        from . import trace as tracemod
        entries += tracemod.entry_points(args.trace_path)
    resimo = scanmod.scan(data, entries, follow_prologues=args.prologues,
                          seed_pointers=args.seed_pointers)

    if args.cmd == "scan":
        stats = collect_stats(resimo, data)
        print(json.dumps({k: v for k, v in stats.items() if k != "largest_functions"
                          and k != "unsupported_by_mnemonic"}, indent=2))
        print("\nNicht unterstuetzt (Top 20):")
        for k, v in list(stats["unsupported_by_mnemonic"].items())[:20]:
            print("  %-12s %6d" % (k, v))
        print("\nGroesste ARM-Funktionen:")
        for d in stats["largest_functions"]:
            print("  0x%08X: %5d Instruktionen (%d nicht unterstuetzt)"
                  % (d["addr"], d["insns"], d["unsupported"]))
        if args.json:
            with open(args.json, "w", encoding="utf-8") as f:
                json.dump(stats, f, indent=2)
    else:
        n, nt = generate(resimo, args.out, args.max_funcs,
                         trace=args.trace_flag, data=data)
        print("geschrieben: %s (%d ARM-Funktionen, %d Thumb-Stubs)" % (args.out, n, nt))


if __name__ == "__main__":
    main()
