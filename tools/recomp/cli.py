"""Kommandozeile des Recompiler-MVP.

  python -m recomp.cli scan --in out/exefs/code_decomp.bin
  python -m recomp.cli gen  --in out/exefs/code_decomp.bin --out build/generated.c
"""
import argparse
import json
import os

from . import isa, emit
from . import scan as scanmod


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
            thumb_ins += sum(len(b.insns) for b in fn.blocks)
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
        "unsupported_pct": round(100.0 * sum(unsupported.values()) / max(1, arm_ins), 2),
        "unsupported_by_mnemonic": dict(sorted(unsupported.items(), key=lambda kv: -kv[1])),
        "visited_bytes": sum(res.visited.values()),
        "text_bytes": len(data),
        "coverage_pct": round(100.0 * sum(res.visited.values()) / len(data), 2),
        "largest_functions": sorted(per_func, key=lambda d: -d["insns"])[:15],
    }


def generate(res, path, max_funcs=None):
    u32 = lambda v: v & 0xFFFFFFFF
    needed = set()
    for fn in res.functions.values():
        for call, _ in fn.calls:
            needed.add(call)
    all_addrs = sorted(set(res.functions) | needed)
    funcs = [res.functions[a] for a in all_addrs if a in res.functions and res.functions[a].blocks]
    if max_funcs:
        funcs = funcs[:max_funcs]
    generated = set(fn.addr for fn in funcs)

    out = ["/* generiert vom Recompiler-MVP */", '#include "arm_cpu.h"', ""]
    for a in all_addrs:
        out.append("void fn_%08X(ArmCpu *c);" % u32(a))
    out.append("")

    n_arm = 0
    n_thumb = 0
    for fn in funcs:
        if fn.thumb:
            n_thumb += 1
            out.append("void fn_%08X(ArmCpu *c) {" % u32(fn.addr))
            out.append('  arm_unimplemented(c, 0x%08Xu, 0, "thumb");' % u32(fn.addr))
            out.append("}")
            out.append("")
            continue
        n_arm += 1
        em = emit.Emitter(fn.addr)
        out.append("void fn_%08X(ArmCpu *c) {" % u32(fn.addr))
        out.append("  goto %s;" % em.label(fn.blocks[0].addr))
        for blk in fn.blocks:
            out.append("%s:" % em.label(blk.addr))
            for ins in blk.insns:
                lines, _ = instruction_lines(em, ins)
                for ln in lines:
                    out.append("  " + ln)
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

    p_gen = sub.add_parser("gen")
    p_gen.add_argument("--in", dest="inp", required=True)
    p_gen.add_argument("--out", required=True)
    p_gen.add_argument("--prologues", action="store_true")
    p_gen.add_argument("--max-funcs", type=int)

    args = ap.parse_args(argv)
    data = load(args.inp)
    resimo = scanmod.scan(data, [(isa.ARM_BASE, "entry", False)], follow_prologues=args.prologues)

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
        n, nt = generate(resimo, args.out, args.max_funcs)
        print("geschrieben: %s (%d ARM-Funktionen, %d Thumb-Stubs)" % (args.out, n, nt))


if __name__ == "__main__":
    main()
