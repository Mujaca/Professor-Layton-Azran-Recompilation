"""Funktions-Differenztest: recompilierter Port-Code vs. Originalbytes in Unicorn.

Der Port schreibt mit ``ARM_DUMP=0xADDR:datei`` (Diagnose, nur lesend) beim
ersten Erreichen eines Basisblocks Register, Flags und alle nichtleeren
4-KiB-Seiten des Gast-Speichers nach ``scratch/``. Dieses Werkzeug laedt den
Schnappschuss in Unicorn, fuehrt dort die Originalbytes ab derselben Adresse
aus und ruft im Port-Build dieselbe Funktion (``fn_XXXX`` aus
``build/gen_boot.c``) mit identischem Anfangszustand auf. Danach werden
Register, Flags und Speicher verglichen.

Beispiele:

  python tools/func_difftest.py dump --watch 0x0016A500 --out scratch/fd/snap.bin
  python tools/func_difftest.py diff --snapshot scratch/fd/snap.bin
  python tools/func_difftest.py diff --snapshot snap.bin --limit-blocks 2000000

Exit-Code 0 = Port und Original verhalten sich gleich, 1 = Unterschied
(oder der Port terminiert nicht, wo das Original zurueckkehrt).

Das Werkzeug ist reines Testwerkzeug; es veraendert keinen Gastzustand.
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "out", "funcdiff")
CODE_BASE = 0x00100000
MEM_SIZE = 0x30000000
SENTINEL = 0xE0000000

MAGIC = b"ARMDMP01"
HDR = 8 + 4 + 16 * 4 + 10 * 4
FLAG_NAMES = ("n", "z", "c", "v", "qflag", "ge", "t", "cpsr_mode",
              "tpidruro", "fpscr")


# --------------------------------------------------------------------------
# Schnappschuss-Format (runtime/cpu/arm_trace.c: dump_state)
# --------------------------------------------------------------------------
def read_snapshot(path):
    data = open(path, "rb").read()
    if data[:8] != MAGIC:
        raise SystemExit("%s: kein ARMDMP01-Schnappschuss" % path)
    pos = 8
    pc, = struct.unpack_from("<I", data, pos)
    pos += 4
    regs = list(struct.unpack_from("<16I", data, pos))
    pos += 64
    vals = list(struct.unpack_from("<10I", data, pos))
    pos += 40
    flags = dict(zip(FLAG_NAMES, vals))
    nreg, = struct.unpack_from("<I", data, pos)
    pos += 4
    regions = []
    for _ in range(nreg):
        addr, size = struct.unpack_from("<II", data, pos)
        pos += 8
        regions.append((addr, data[pos:pos + size]))
        pos += size
    return {"path": path, "pc": pc, "regs": regs, "flags": flags,
            "regions": regions}


def pages_of(regions):
    out = {}
    for addr, blob in regions:
        for off in range(0, len(blob), 4096):
            out[addr + off] = blob[off:off + 4096]
    return out


# --------------------------------------------------------------------------
# Unicorn-Referenzlauf
# --------------------------------------------------------------------------
def run_unicorn(snap, entry, limit, timeout):
    from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_THUMB,
                         UC_MODE_LITTLE_ENDIAN, UC_HOOK_CODE)
    import unicorn.arm_const as U

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_LITTLE_ENDIAN)
    # Der Port bildet den Gast-Speicher flach ab; 0x20000000 ist ein Alias.
    uc.mem_map(0, 0x20000000)
    code = open(os.path.join(ROOT, "out", "exefs", "code_decomp.bin"),
                "rb").read()
    uc.mem_write(CODE_BASE, code)
    for addr, blob in snap["regions"]:
        uc.mem_write(addr, blob)

    f = snap["flags"]
    cpsr = (f["cpsr_mode"] & 0x1F) | ((f["t"] & 1) << 5)
    cpsr |= (f["n"] & 1) << 31
    cpsr |= (f["z"] & 1) << 30
    cpsr |= (f["c"] & 1) << 29
    cpsr |= (f["v"] & 1) << 28
    cpsr |= (f["qflag"] & 1) << 27
    cpsr |= (f["ge"] & 0xF) << 16
    uc.reg_write(U.UC_ARM_REG_CPSR, cpsr)
    for i in range(16):
        uc.reg_write(getattr(U, "UC_ARM_REG_R%d" % i), snap["regs"][i])
    uc.reg_write(U.UC_ARM_REG_LR, SENTINEL)

    state = {"n": 0, "stop": None}
    begin = entry | (1 if f["t"] else 0)
    trace = []

    def hook(uc_, address, size, user):
        state["n"] += 1
        if trace is not None:
            cs = uc_.reg_read(U.UC_ARM_REG_CPSR)
            trace.append((address,
                          [uc_.reg_read(getattr(U, "UC_ARM_REG_R%d" % i))
                           for i in range(16)],
                          ((cs >> 31) & 1, (cs >> 30) & 1, (cs >> 29) & 1,
                           (cs >> 28) & 1, (cs >> 16) & 0xF)))
        if state["n"] > limit:
            state["stop"] = "limit"
            uc_.emu_stop()

    uc.hook_add(UC_HOOK_CODE, hook)
    try:
        uc.emu_start(begin, SENTINEL)
    except Exception as e:  # Unicorn meldet ungemappte Zugriffe/Fehler
        state["stop"] = "exception: %s" % e
    if state["stop"] is None and uc.reg_read(U.UC_ARM_REG_PC) != SENTINEL:
        state["stop"] = "pc=0x%08X" % uc.reg_read(U.UC_ARM_REG_PC)
    regs = [uc.reg_read(getattr(U, "UC_ARM_REG_R%d" % i)) for i in range(16)]
    cpsr = uc.reg_read(U.UC_ARM_REG_CPSR)
    f2 = {"n": (cpsr >> 31) & 1, "z": (cpsr >> 30) & 1,
          "c": (cpsr >> 29) & 1, "v": (cpsr >> 28) & 1,
          "qflag": (cpsr >> 27) & 1, "ge": (cpsr >> 16) & 0xF,
          "t": (cpsr >> 5) & 1, "cpsr_mode": cpsr & 0x1F}
    return {"regs": regs, "flags": f2, "stop": state["stop"],
            "insns": state["n"], "uc": uc, "trace": trace}


def unicorn_pages(uc, addrs):
    out = {}
    for a in addrs:
        try:
            out[a] = bytes(uc.mem_read(a, 4096))
        except Exception:
            out[a] = b"\x00" * 4096
    return out


# --------------------------------------------------------------------------
# Port-Lauf (Harness gegen build/gen_boot.o)
# --------------------------------------------------------------------------
HARNESS = r'''/* generiert von tools/func_difftest.py */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arm_cpu.h"

#define MEM_SIZE 0x30000000u
void fn_%(entry)08X(ArmCpu *c);

uint32_t arm_last_block = 0;
static jmp_buf g_jmp;
static unsigned long g_blocks = 0;
static unsigned long g_limit = 0;
static FILE *g_trace = NULL;

void arm_trace_block(ArmCpu *c, uint32_t pc, uint32_t thumb) {
    (void)thumb;
    arm_last_block = pc;
    if (g_trace != NULL) {
        uint32_t v[22];
        v[0] = pc;
        for (int i = 0; i < 16; i++) v[1 + i] = c->r[i];
        v[17] = c->n; v[18] = c->z; v[19] = c->c; v[20] = c->v; v[21] = c->ge;
        fwrite(v, 4, 22, g_trace);
    }
    if (g_limit != 0 && ++g_blocks > g_limit) longjmp(g_jmp, 1);
}
void arm_trace_svc(uint32_t imm) { (void)imm; }
int arm_ldrpc_log_on(void) { return 0; }
void arm_ldrpc_log(unsigned t) { (void)t; }

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static void wr32(FILE *f, uint32_t v) { fwrite(&v, 1, 4, f); }

static int page_nonzero(const unsigned char *p) {
    uint64_t acc = 0;
    for (int i = 0; i < 4096; i += 8) { uint64_t w; memcpy(&w, p + i, 8); acc |= w; }
    return acc != 0;
}

static void write_snapshot(const char *path, ArmCpu *c) {
    FILE *f = fopen(path, "wb");
    uint32_t regions[4096][2];
    unsigned n = 0;
    if (!f) { perror(path); return; }
    fwrite("ARMDMP01", 1, 8, f);
    wr32(f, arm_last_block);
    for (int i = 0; i < 16; i++) wr32(f, c->r[i]);
    wr32(f, c->n); wr32(f, c->z); wr32(f, c->c); wr32(f, c->v);
    wr32(f, c->qflag); wr32(f, c->ge); wr32(f, c->t);
    wr32(f, c->cpsr_mode); wr32(f, c->tpidruro); wr32(f, c->fpscr);
    wr32(f, 0);
    for (uint32_t a = 0; a + 4096u <= c->mem_size && a < 0x20000000u; a += 4096u) {
        if (a >= 0x00100000u && a < 0x00400000u) continue;
        if (!page_nonzero(c->mem + a)) continue;
        if (n > 0 && regions[n-1][0] + regions[n-1][1] == a) regions[n-1][1] += 4096u;
        else if (n < 4096u) { regions[n][0] = a; regions[n][1] = 4096u; n++; }
    }
    /* Codebereich fuer den Vergleich nicht noetig; Unicorn laedt ihn selbst. */
    for (unsigned i = 0; i < n; i++) {
        wr32(f, regions[i][0]); wr32(f, regions[i][1]);
        fwrite(c->mem + regions[i][0], 1, regions[i][1], f);
    }
    fseek(f, %(hdr)d, SEEK_SET);
    wr32(f, n);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: snap_in snap_out limit [trace]\n"); return 2; }
    const char *in = argv[1], *outp = argv[2];
    if (argc > 4) g_trace = fopen(argv[4], "wb");
    g_limit = strtoul(argv[3], NULL, 0);
    FILE *f = fopen(in, "rb");
    if (!f) { perror(in); return 2; }
    unsigned char hdr[%(hdr)d + 4];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr ||
        memcmp(hdr, "ARMDMP01", 8) != 0) { fprintf(stderr, "bad snapshot\n"); return 2; }
    uint32_t nreg = rd32(hdr + %(hdr)d);
    unsigned char *mem = (unsigned char *)calloc(1, MEM_SIZE);
    if (!mem) { fprintf(stderr, "kein Speicher\n"); return 2; }
    for (uint32_t i = 0; i < nreg; i++) {
        unsigned char rh[8];
        if (fread(rh, 1, 8, f) != 8) return 2;
        uint32_t a = rd32(rh), sz = rd32(rh + 4);
        if (a + sz <= MEM_SIZE && fread(mem + a, 1, sz, f) != sz) return 2;
    }
    fclose(f);
    {
        FILE *cf = fopen("out/exefs/code_decomp.bin", "rb");
        if (cf) { fread(mem + 0x00100000u, 1, 0x00300000u, cf); fclose(cf); }
    }
    ArmCpu c;
    arm_cpu_init(&c, mem, MEM_SIZE);
    for (int i = 0; i < 16; i++) {
        uint32_t v; memcpy(&v, hdr + 12 + i * 4, 4); c.r[i] = v;
    }
    c.n = rd32(hdr + 76); c.z = rd32(hdr + 80); c.c = rd32(hdr + 84);
    c.v = rd32(hdr + 88); c.qflag = rd32(hdr + 92); c.ge = rd32(hdr + 96);
    c.t = rd32(hdr + 100); c.cpsr_mode = rd32(hdr + 104);
    c.tpidruro = rd32(hdr + 108); c.fpscr = rd32(hdr + 112);
    c.r[14] = 0xE0000000u;
    if (setjmp(g_jmp) == 0) {
        fn_%(entry)08X(&c);
        printf("RETURNED\n");
    } else {
        printf("LIMIT blocks=%%lu lastpc=0x%%08X\n", g_blocks, arm_last_block);
    }
    if (c.aborted) printf("ABORTED\n");
    write_snapshot(outp, &c);
    printf("REGS");
    for (int i = 0; i < 16; i++) printf(" %%08X", c.r[i]);
    printf("\nFLAGS %%u %%u %%u %%u %%u %%X %%u %%u\n", c.n, c.z, c.c, c.v,
           c.qflag, c.ge, c.t, c.cpsr_mode);
    return 0;
}
'''


def build_harness(entry, snapshot):
    os.makedirs(OUT, exist_ok=True)
    src = os.path.join(OUT, "harness.c")
    with open(src, "w", encoding="utf-8") as f:
        f.write(HARNESS % {"entry": entry, "hdr": HDR,
                           "nreg_off": HDR - 4})
    obj = os.path.join(ROOT, "build", "gen_boot.o")
    if not os.path.exists(obj):
        raise SystemExit("build/gen_boot.o fehlt - zuerst 'python tools/build.py'")
    exe = os.path.join(OUT, "harness.exe")
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), "zig.exe")
    cmd = [zig, "cc", "-std=c11", "-O1", "-I", os.path.join(ROOT, "runtime", "cpu"),
           src, os.path.join(ROOT, "runtime", "cpu", "arm_cpu.c"), obj,
           "-o", exe]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit("Harness kompiliert nicht")
    return exe


def run_port(entry, snap_path, limit):
    exe = build_harness(entry, snap_path)
    post = os.path.join(OUT, "post.bin")
    trace = os.path.join(OUT, "port_blocks.bin")
    p = subprocess.run([exe, snap_path, post, str(limit), trace], cwd=ROOT,
                       capture_output=True, text=True, timeout=600)
    lines = [l for l in p.stdout.splitlines() if l]
    status = next((l for l in lines if l.startswith(("RETURNED", "LIMIT", "ABORTED"))),
                  "?")
    return {"status": status, "stdout": p.stdout, "stderr": p.stderr,
            "post": post if os.path.exists(post) else None, "trace": trace}


def read_port_trace(path):
    data = open(path, "rb").read()
    out = []
    for off in range(0, len(data) - 88 + 1, 88):
        vals = struct.unpack_from("<22I", data, off)
        out.append((vals[0], list(vals[1:17]),
                    tuple(vals[17:22])))
    return out


def first_trace_diff(utrace, ptrace):
    """Findet den ersten Port-Block, dessen Eingangsregister von Unicorn
    abweichen, bzw. den ersten Block, den Unicorn an dieser Stelle nicht
    erreicht (Kontrollfluss-Abweichung im Vorgaengerblock).

    Bloecke, die im generierten Code als Sprungziel nur beim Ruecksprung
    markiert sind (Schleifen), traegt der Port erst ab der zweiten Iteration
    ein. Deshalb werden fuer jede Blockadresse mehrere Unicorn-Treffer
    geprueft, bevor eine Abweichung gemeldet wird."""
    j = 0
    for pi, (pc, regs, pflags) in enumerate(ptrace):
        k = j
        found = None
        first = None
        while k < len(utrace):
            if utrace[k][0] == pc:
                if first is None:
                    first = k
                ur, uf = utrace[k][1], utrace[k][2]
                if all((ur[r] & 0xFFFFFFFF) == (regs[r] & 0xFFFFFFFF)
                       for r in range(15)) and uf == pflags:
                    found = k
                    break
                if k - first > 2:
                    break
            k += 1
        if found is not None:
            j = found + 1
            continue
        if first is None:
            return ("Kontrollfluss", pi, pc, None, None, None)
        ur, uf = utrace[first][1], utrace[first][2]
        for r in range(15):
            if (ur[r] & 0xFFFFFFFF) != (regs[r] & 0xFFFFFFFF):
                return ("Register", pi, pc, r, ur[r] & 0xFFFFFFFF,
                        regs[r] & 0xFFFFFFFF)
        if uf != pflags:
            return ("Flags", pi, pc, None, uf, pflags)
        return ("Kontrollfluss", pi, pc, None, None, None)
    return None


# --------------------------------------------------------------------------
# Vergleich
# --------------------------------------------------------------------------
def compare(a_regs, a_flags, a_pages, b_regs, b_flags, b_pages, limit=20):
    diffs = []
    for i in range(15):
        if a_regs[i] != b_regs[i]:
            diffs.append("r%d: unicorn=0x%08X port=0x%08X"
                         % (i, a_regs[i], b_regs[i]))
    for k in FLAG_NAMES[:6]:
        if a_flags.get(k) != b_flags.get(k):
            diffs.append("%s: unicorn=%s port=%s"
                         % (k, a_flags.get(k), b_flags.get(k)))
    page_diffs = []
    for addr in sorted(set(a_pages) | set(b_pages)):
        pa = a_pages.get(addr)
        pb = b_pages.get(addr)
        if pa == pb:
            continue
        if pa is None:
            pa = b"\x00" * 4096
        if pb is None:
            pb = b"\x00" * 4096
        off = next((i for i in range(4096) if pa[i] != pb[i]), 0)
        page_diffs.append((addr, off, pa, pb))
    for addr, off, pa, pb in page_diffs[:limit]:
        diffs.append("mem 0x%08X (erste Abweichung +0x%X): unicorn=%s port=%s"
                     % (addr, off,
                        pa[max(0, off - 4):off + 12].hex(),
                        pb[max(0, off - 4):off + 12].hex()))
    if len(page_diffs) > limit:
        diffs.append("... %d weitere Speicherseiten" % (len(page_diffs) - limit))
    return diffs, len(page_diffs)


def cmd_dump(a):
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    env = {k: v for k, v in os.environ.items()
           if not (k.startswith("HLE_") or k.startswith("ARM_") or
                   k.startswith("PORT_"))}
    env["HLE_LOG"] = "1"
    env["ARM_DUMP"] = "0x%08X:%s" % (a.watch, os.path.abspath(a.out))
    log = a.out + ".log"
    with open(log, "wb") as f:
        try:
            subprocess.run([a.exe, os.path.join(ROOT, "out", "romfs")], cwd=ROOT,
                           env=env, stdout=f, stderr=subprocess.STDOUT,
                           timeout=a.timeout)
        except subprocess.TimeoutExpired:
            pass
    if not os.path.exists(a.out):
        print("Kein Schnappschuss: 0x%08X wurde in %d s nicht erreicht (Log: %s)"
              % (a.watch, a.timeout, log))
        return 1
    snap = read_snapshot(a.out)
    print("Schnappschuss %s: pc=0x%08X r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X "
          "sp=0x%08X regions=%d"
          % (a.out, snap["pc"], snap["regs"][0], snap["regs"][1],
             snap["regs"][2], snap["regs"][3], snap["regs"][13],
             len(snap["regions"])))
    return 0


def cmd_diff(a):
    snap = read_snapshot(a.snapshot)
    entry = a.entry or snap["pc"]
    if not a.entry and snap["pc"] != entry:
        entry = snap["pc"]
    print("Funktion 0x%08X, Modus %s, Start r0=0x%08X r1=0x%08X r2=0x%08X "
          "r3=0x%08X sp=0x%08X"
          % (entry, "Thumb" if snap["flags"]["t"] else "ARM",
             snap["regs"][0], snap["regs"][1], snap["regs"][2],
             snap["regs"][3], snap["regs"][13]))
    u = run_unicorn(snap, entry, a.unicorn_limit, a.timeout)
    print("unicorn: %s (%d Instruktionen)"
          % (u["stop"] or "zurueckgekehrt", u["insns"]))
    port = run_port(entry, os.path.abspath(a.snapshot), a.limit_blocks)
    print("port:    %s" % port["status"])
    if port["stderr"].strip():
        print("port stderr: %s" % port["stderr"].strip().splitlines()[-1])
    ptrace = read_port_trace(port["trace"]) if os.path.exists(port["trace"]) else []
    if a.trace and ptrace and u["trace"]:
        d = first_trace_diff(u["trace"], ptrace)
        if d is None:
            print("Spur: kein Unterschied an Blockgrenzen")
        else:
            kind, pi, pc, r, uv, pv = d
            prev = ptrace[pi - 1][0] if pi > 0 else entry
            if kind == "Register":
                print("Spur: erster Unterschied an Block 0x%08X (Eintritt): "
                      "r%d unicorn=0x%08X port=0x%08X -> Rechenfehler im "
                      "Vorgaengerblock 0x%08X"
                      % (pc, r, uv, pv, prev))
            elif kind == "Flags":
                print("Spur: erster Flag-Unterschied an Block 0x%08X: "
                      "n,z,c,v,ge unicorn=%s port=%s -> Vorgaengerblock "
                      "0x%08X" % (pc, uv, pv, prev))
            else:
                print("Spur: Kontrollfluss weicht ab - Block 0x%08X wird im "
                      "Port erreicht, in Unicorn nicht (Vorgaengerblock "
                      "0x%08X)" % (pc, prev))
            rng = a.context or 24
            try:
                import capstone
                data = open(os.path.join(ROOT, "out", "exefs",
                                         "code_decomp.bin"), "rb").read()
                md = capstone.Cs(capstone.CS_ARCH_ARM,
                                 capstone.CS_MODE_THUMB if snap["flags"]["t"]
                                 else capstone.CS_MODE_ARM)
                off = prev - CODE_BASE
                print("Disassemblierung ab 0x%08X:" % prev)
                for ins in md.disasm(data[off:off + rng * 4], prev):
                    print("  %08X  %-7s %s" % (ins.address, ins.mnemonic,
                                               ins.op_str))
            except Exception as e:
                print("(Disassemblierung nicht moeglich: %s)" % e)
    if port["post"] is None:
        print("Kein Port-Schnappschuss - Lauf abgebrochen")
        return 1
    post = read_snapshot(port["post"])
    pre_pages = pages_of(snap["regions"])
    port_pages = pages_of(post["regions"])
    uni_pages = unicorn_pages(u["uc"], set(pre_pages) | set(port_pages))
    diffs, npages = compare(u["regs"], u["flags"], uni_pages,
                            post["regs"], post["flags"], port_pages)
    ok = not diffs and u["stop"] is None and port["status"] == "RETURNED"
    if ok:
        print("OK: Register, Flags und Speicher identisch.")
        return 0
    print("ABWEICHUNGEN (%d):" % len(diffs))
    for d in diffs[:40]:
        print("  " + d)
    if npages:
        print("  Speicherseiten abweichend: %d" % npages)
    return 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dump", help="Port-Lauf mit ARM_DUMP-Schnappschuss")
    d.add_argument("--watch", type=lambda s: int(s, 0), required=True,
                   help="Basisblock-Adresse, z. B. 0x0016A500")
    d.add_argument("--out", required=True)
    d.add_argument("--timeout", type=int, default=40)
    d.add_argument("--exe", default=os.path.join(ROOT, "out", "port.exe"))
    d.set_defaults(func=cmd_dump)

    f = sub.add_parser("diff", help="Schnappschuss gegen Unicorn und Port vergleichen")
    f.add_argument("--snapshot", required=True)
    f.add_argument("--entry", type=lambda s: int(s, 0), default=None)
    f.add_argument("--limit-blocks", type=int, default=20000000)
    f.add_argument("--unicorn-limit", type=int, default=50000000)
    f.add_argument("--trace", action="store_true",
                   help="ersten Unterschied an Blockgrenzen lokalisieren")
    f.add_argument("--context", type=int, default=24,
                   help="Instruktionen im Vorgaengerblock disassemblieren")
    f.add_argument("--timeout", type=int, default=180)
    f.set_defaults(func=cmd_diff)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
