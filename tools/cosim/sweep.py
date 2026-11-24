"""Recompiler-Sweep: welche ausgeführten Gastfunktionen weichen von Unicorn ab?

Der Sweep verbindet drei vorhandene Bausteine:

1. ``HLE_PERFMAP_FILE``-Lauf des Ports (Standard 1200 Frames) liefert je
   Basisblock ``(pc, thread, count)``.
2. Die Eintragstabelle in ``build/gen_boot.c`` (``{ 0xADDR, fn_ADDR }``) sagt,
   welche Adressen Funktions-Einstiege sind. Geschnitten mit der Perfmap
   ergibt das die im natürlichen Boot tatsächlich ausgeführten Funktionen,
   priorisiert nach Thread (Hauptthread/Spiellogik zuerst) und
   Ausführungszahl. Alle selten (<= ``--rare``) ausgeführten Funktionen
   kommen zusätzlich nach vorne (Initialisierungspfade).
3. Pro Funktion wird ein ``ARM_DUMP``-Schnappschuss am Funktions-Einstieg
   genommen (Diagnose, nur lesend) und blockgleichschrittig mit den
   Originalbytes in Unicorn verglichen (Port-Code aus ``build/gen_boot.o``).

Ergebnisklassen:

  gleich                     Blockfolge, Register, Flags, Speicher identisch
  unicorn-ende:svc|tls|mem   Unicorn endet an einem SVC, CP15-/TLS-Zugriff oder
  |limit|sonst               nicht abgebildeten Speicher - kein Recompiler-
                             Befund (der Port kann dort weiterlaufen)
  abweichung:block|ende      echte Abweichung (Blockeingang bzw. Endzustand)
  port-limit|port-abort      Port-Code terminiert nicht / bricht ab
  snapshot-fehlend           Einstieg im Perfmap-Lauf nicht erreichbar

Eine Abweichung gilt als ``lock-verdacht`` (Fremdthread-/Lock-Speicher), wenn
der Vorgängerblock einen Exklusiv-Zugriff (``ldrex``/``strex``/``swp``)
enthält oder ein ``ldr`` von einem Wort lädt, das zwischen Unicorn und Port
abweicht. Alle anderen Abweichungen sind Kandidaten für einen
Recompiler-Fehler und werden je Funktion in ``fail_<addr>.txt`` abgelegt.

Laufzeit: je Funktion greift ``--per-func`` (Standard 60 s, Deckel für
Schnappschuss + Port-Harness), zusätzlich ein Gesamtbudget ``--budget``.

Beispiele:

  python tools/cosim/sweep.py                      # Perfmap + Sweep
  python tools/cosim/sweep.py --reuse-perf scratch/sweep/perf.txt --max-funcs 50
  python tools/cosim/sweep.py --only 0x001016B8,0x0028F04C
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)
import func_difftest as fd  # noqa: E402

ENTRY_RE = re.compile(r"^    \{ 0x([0-9A-F]{8})u, fn_([0-9A-F]{8}) \},", re.M)
EMPTY_RE = re.compile(r"^void fn_([0-9A-F]{8})\(ArmCpu \*c\) \{\n  "
                      r"arm_unimplemented", re.M)


# ---------------------------------------------------------------- Perfmap
def parse_perfmap(path):
    rows = {}
    totals = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("pc "):
                fields = line.split()
                rows[(int(fields[1], 16), int(fields[2]))] = int(fields[3])
            elif line.startswith("# thread "):
                fields = line.split()
                totals[int(fields[2])] = int(fields[3])
    return rows, totals


def perfmap_run(root, frames, out):
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    env = {k: v for k, v in os.environ.items()
           if not (k.startswith("HLE_") or k.startswith("ARM_")
                   or k.startswith("PORT_"))}
    env["HLE_PERFMAP_FILE"] = os.path.abspath(out)
    exe = os.path.join(root, "out", "port.exe")
    log = os.path.abspath(out) + ".log"
    with open(log, "wb") as f:
        try:
            subprocess.run([exe, os.path.join(root, "out", "romfs"),
                            "--frames", str(frames)], cwd=root, env=env,
                           stdout=f, stderr=subprocess.STDOUT, timeout=900)
        except subprocess.TimeoutExpired:
            pass
    if not os.path.exists(out):
        raise SystemExit("Perfmap fehlt: %s (Log %s)" % (out, log))
    return out


def parse_entries(root):
    with open(os.path.join(root, "build", "gen_boot.c"), "r",
              encoding="utf-8", errors="replace") as f:
        src = f.read()
    entries = {}
    for addr, fn in ENTRY_RE.findall(src):
        entries[int(addr, 16)] = int(fn, 16)
    empty = {int(a, 16) for a in EMPTY_RE.findall(src)}
    return entries, empty


def build_plan(rows, totals, entries, empty, rare, main_pc=0x00100000):
    """Priorisierte Liste ``(addr, thread, count, gruppe)``."""
    main_t = None
    for (pc, t) in rows:
        if pc == main_pc:
            main_t = t
            break
    if main_t is None and totals:
        main_t = max(totals, key=totals.get)
    per_addr = {}
    for (pc, t), c in rows.items():
        per_addr.setdefault(pc, []).append((t, c))
    rare_main, hot_main, rare_other, hot_other = [], [], [], []
    for addr in entries:
        if addr in empty or addr not in per_addr:
            continue
        by_thread = per_addr[addr]
        tmax, cmax = max(by_thread, key=lambda x: x[1])
        total = sum(c for _t, c in by_thread)
        item = (addr, tmax, total, cmax)
        main = (tmax == main_t)
        if total <= rare:
            (rare_main if main else rare_other).append(item)
        else:
            (hot_main if main else hot_other).append(item)
    for lst in (rare_main, rare_other):
        lst.sort(key=lambda x: x[2])            # 1x zuerst
    for lst in (hot_main, hot_other):
        lst.sort(key=lambda x: -x[2])           # heiss zuerst
    plan = ([(a, t, c, "selten/haupt") for a, t, _n, c in rare_main]
            + [(a, t, c, "heiss/haupt") for a, t, _n, c in hot_main]
            + [(a, t, c, "selten/neben") for a, t, _n, c in rare_other]
            + [(a, t, c, "heiss/neben") for a, t, _n, c in hot_other])
    return plan, main_t


# ---------------------------------------------------------------- Snapshot
def snapshot(root, addr, path, timeout):
    """Port bis zum ersten Treffer von ``addr`` laufen lassen, dann beenden."""
    env = {k: v for k, v in os.environ.items()
           if not (k.startswith("HLE_") or k.startswith("ARM_")
                   or k.startswith("PORT_"))}
    env["ARM_DUMP"] = "0x%08X:%s" % (addr, os.path.abspath(path))
    try:
        os.remove(path)
    except OSError:
        pass
    log = path + ".log"
    exe = os.path.join(root, "out", "port.exe")
    t0 = time.time()
    with open(log, "wb") as f:
        p = subprocess.Popen([exe, os.path.join(root, "out", "romfs")],
                             cwd=root, env=env, stdout=f,
                             stderr=subprocess.STDOUT)
        last, stable = -1, 0
        try:
            while time.time() - t0 < timeout:
                if p.poll() is not None:
                    break
                try:
                    size = os.path.getsize(path)
                except OSError:
                    size = -1
                if size > 0 and size == last:
                    stable += 1
                    if stable >= 4:          # 4 * 50 ms ohne Wachstum
                        break
                else:
                    stable = 0
                last = size
                time.sleep(0.05)
        finally:
            if p.poll() is None:
                p.kill()
            p.wait()
    return (os.path.abspath(path) if os.path.exists(path) else None,
            time.time() - t0)


# ---------------------------------------------------------------- Unicorn
def _flags(cs):
    return ((cs >> 31) & 1, (cs >> 30) & 1, (cs >> 29) & 1,
            (cs >> 28) & 1, (cs >> 16) & 0xF)


def unicorn_run(snap, limit, starts):
    """Unicorn bis zur Rueckkehr; Spur nur an Port-Blockeinstiegen (Speicher)."""
    from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_LITTLE_ENDIAN,
                         UC_HOOK_CODE)
    import unicorn.arm_const as U

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_LITTLE_ENDIAN)
    uc.mem_map(0, 0x20000000)
    with open(os.path.join(fd.ROOT, "out", "exefs", "code_decomp.bin"),
              "rb") as f:
        uc.mem_write(fd.CODE_BASE, f.read())
    for addr, blob in snap["regions"]:
        uc.mem_write(addr, blob)
    f = snap["flags"]
    cpsr = (f["cpsr_mode"] & 0x1F) | ((f["t"] & 1) << 5)
    cpsr |= (f["n"] & 1) << 31 | (f["z"] & 1) << 30
    cpsr |= (f["c"] & 1) << 29 | (f["v"] & 1) << 28
    cpsr |= (f["qflag"] & 1) << 27 | (f["ge"] & 0xF) << 16
    uc.reg_write(U.UC_ARM_REG_CPSR, cpsr)
    for i in range(16):
        uc.reg_write(getattr(U, "UC_ARM_REG_R%d" % i), snap["regs"][i])
    uc.reg_write(U.UC_ARM_REG_LR, fd.SENTINEL)
    # TLS-Register (mrc p15, 0, rX, c13, c0, 3) und FPSCR wie im Gastlauf,
    # damit TLS-/Float-Funktionen nicht am fehlenden Kontext scheitern.
    uc.reg_write(U.UC_ARM_REG_C13_C0_3, f["tpidruro"])
    uc.reg_write(U.UC_ARM_REG_FPSCR, f["fpscr"])
    # FPEXC.EN (Bit 30) und CP10/CP11 in CPACR: ohne beides lehnt Unicorn
    # VFP-Instruktionen als "Invalid instruction" ab (vgl. vfp_difftest.py).
    uc.reg_write(U.UC_ARM_REG_FPEXC, 0x40000000)
    uc.reg_write(U.UC_ARM_REG_C1_C0_2,
                 uc.reg_read(U.UC_ARM_REG_C1_C0_2) | (0xF << 20))

    begin = snap["pc"] | (1 if f["t"] else 0)
    state = {"n": 0, "stop": None, "last": None, "last_t": False}
    trace = []

    def hook(uc_, address, size, user):
        state["n"] += 1
        cs = uc_.reg_read(U.UC_ARM_REG_CPSR)
        state["last"] = address
        state["last_t"] = bool(cs & 0x20)
        if address in starts:
            trace.append((address,
                          [uc_.reg_read(getattr(U, "UC_ARM_REG_R%d" % i))
                           for i in range(16)], _flags(cs)))
        if state["n"] > limit:
            state["stop"] = "limit"
            uc_.emu_stop()

    uc.hook_add(UC_HOOK_CODE, hook)
    try:
        uc.emu_start(begin, fd.SENTINEL)
    except Exception as e:
        state["stop"] = "exception: %s" % e
    if state["stop"] is None and uc.reg_read(U.UC_ARM_REG_PC) != fd.SENTINEL:
        state["stop"] = "pc=0x%08X" % uc.reg_read(U.UC_ARM_REG_PC)
    regs = [uc.reg_read(getattr(U, "UC_ARM_REG_R%d" % i)) for i in range(16)]
    cs = uc.reg_read(U.UC_ARM_REG_CPSR)
    f2 = {"n": (cs >> 31) & 1, "z": (cs >> 30) & 1,
          "c": (cs >> 29) & 1, "v": (cs >> 28) & 1,
          "qflag": (cs >> 27) & 1, "ge": (cs >> 16) & 0xF,
          "t": (cs >> 5) & 1, "cpsr_mode": cs & 0x1F}
    return {"regs": regs, "flags": f2, "stop": state["stop"],
            "insns": state["n"], "uc": uc, "trace": trace,
            "stop_pc": uc.reg_read(U.UC_ARM_REG_PC),
            "last": state["last"], "last_thumb": state["last_t"]}


def disasm_insn(addr, thumb):
    try:
        import capstone
        with open(os.path.join(fd.ROOT, "out", "exefs", "code_decomp.bin"),
                  "rb") as f:
            code = f.read()
        md = capstone.Cs(capstone.CS_ARCH_ARM,
                         capstone.CS_MODE_THUMB if thumb
                         else capstone.CS_MODE_ARM)
        off = addr - fd.CODE_BASE
        if off < 0 or off + 4 > len(code):
            return ""
        for ins in md.disasm(code[off:off + 4], addr):
            return "%s %s" % (ins.mnemonic, ins.op_str)
    except Exception:
        pass
    return ""


def classify_stop(u, thumb):
    """Unicorn-Ende einordnen: SVC, CP15/TLS, unmapped, Limit, sonst."""
    msg = (u["stop"] or "").lower()
    addr = u["last"] if u.get("last") is not None else u["stop_pc"]
    mode = u.get("last_thumb", thumb)
    ins = disasm_insn(addr, mode)
    if ins.startswith("svc"):
        return "unicorn-ende:svc (%s @0x%08X)" % (ins, addr)
    if ins.startswith(("mrc", "mcr")) or "c13" in ins:
        return "unicorn-ende:tls (%s @0x%08X)" % (ins, addr)
    if "limit" in msg:
        return "unicorn-ende:limit"
    if "memory" in msg or "read" in msg or "write" in msg:
        return "unicorn-ende:mem (%s @0x%08X)" % (ins or "-", addr)
    return "unicorn-ende:sonst (%s @0x%08X)" % (ins or "-", addr)


# ---------------------------------------------------------------- Lock-Check
EXCL = ("ldrex", "ldrexd", "strex", "strexd", "swp", "swpb", "ldaex", "stlex")


def lock_hint(prev_pc, thumb, uni_pages, port_pages, regs):
    """Heuristik für Fremdthread-/Lock-Speicher an der Abweichung."""
    try:
        import capstone
        with open(os.path.join(fd.ROOT, "out", "exefs", "code_decomp.bin"),
                  "rb") as f:
            code = f.read()
        md = capstone.Cs(capstone.CS_ARCH_ARM,
                         capstone.CS_MODE_THUMB if thumb
                         else capstone.CS_MODE_ARM)
        off = prev_pc - fd.CODE_BASE
        ins = list(md.disasm(code[max(0, off - 8):off + 8],
                             max(fd.CODE_BASE, prev_pc - 8)))
    except Exception:
        return None
    if any(i.mnemonic in EXCL for i in ins):
        return "lock (Exklusiv-Zugriff im Vorgaengerblock)"
    if not port_pages:
        return None
    words = {}
    for page in set(uni_pages) | set(port_pages):
        a = uni_pages.get(page) or b"\x00" * 4096
        b = port_pages.get(page) or b"\x00" * 4096
        if a == b:
            continue
        for k in range(0, 4096, 4):
            if a[k:k + 4] != b[k:k + 4] and len(words) < 64:
                words[page + k] = (int.from_bytes(a[k:k + 4], "little"),
                                   int.from_bytes(b[k:k + 4], "little"))
    for i in ins:
        if not i.mnemonic.startswith(("ldr", "ldm")):
            continue
        m = re.search(r"\[(r\d+)(?:, #(0x[0-9a-f]+|\d+))?\]", i.op_str)
        if not m:
            continue
        base = int(m.group(1)[1:])
        off2 = int(m.group(2), 0) if m.group(2) else 0
        a = (regs[base] + off2) & 0xFFFFFFFF
        if a in words:
            return ("fremdthread-speicher (0x%08X von '%s' geladen; "
                    "unicorn=0x%08X port=0x%08X)"
                    % (a, "%s %s" % (i.mnemonic, i.op_str), *words[a]))
    return None


# ---------------------------------------------------------------- Vergleich
def _block_dev(r, addr, snap, thumb, ptrace):
    kind, pi, pc, reg, uv, pv = r
    prev = ptrace[pi - 1][0] if pi > 0 else addr
    return {"class": "abweichung:block", "kind": kind, "pc": pc,
            "prev": prev, "reg": reg, "unicorn_wert": uv, "port_wert": pv,
            "unicorn": None, "snap": snap, "pi": pi,
            "prev_insn": disasm_insn(prev, thumb), "ptrace_len": len(ptrace)}


def trace_diff(utrace, ptrace):
    """Wie ``fd.first_trace_diff``, liefert zusätzlich die Zahl der am Anfang
    übereinstimmenden Port-Blöcke. Wichtig für Funktionen, die einen SVC oder
    TLS-Zugriff ausführen: Unicorn endet dort, der Port läuft weiter. Ein
    nicht mehr auffindbarer Port-Block direkt nach dem letzten
    übereinstimmenden Block ist dann das Unicorn-Ende, keine Abweichung."""
    j = 0
    matched = 0
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
            matched = pi + 1
            continue
        if first is None:
            return ("Kontrollfluss", pi, pc, None, None, None), matched
        ur, uf = utrace[first][1], utrace[first][2]
        for r in range(15):
            if (ur[r] & 0xFFFFFFFF) != (regs[r] & 0xFFFFFFFF):
                return ("Register", pi, pc, r, ur[r] & 0xFFFFFFFF,
                        regs[r] & 0xFFFFFFFF), matched
        if uf != pflags:
            return ("Flags", pi, pc, None, uf, pflags), matched
        return ("Kontrollfluss", pi, pc, None, None, None), matched
    return None, matched


def compare_func(addr, snap_path, limit_blocks, uni_limit, timeout=60.0):
    snap = fd.read_snapshot(snap_path)
    thumb = bool(snap["flags"]["t"])
    port = fd.run_port(addr, os.path.abspath(snap_path), limit_blocks,
                       timeout=timeout)
    ptrace = fd.read_port_trace(port["trace"])
    if not ptrace:
        return {"class": "port-ohne-trace (%s)" % port["status"],
                "snap": snap}
    starts = {pc for pc, _r, _f in ptrace}
    u = unicorn_run(snap, uni_limit, starts)
    d, matched = trace_diff(u["trace"], ptrace)
    if u["stop"] is not None:
        # Unicorn endet (z. B. am SVC): nur den gemeinsamen Prefix vergleichen.
        if d is None or (d[0] == "Kontrollfluss" and d[1] == matched):
            return {"class": classify_stop(u, thumb), "unicorn": u,
                    "snap": snap}
        out = _block_dev(d, addr, snap, thumb, ptrace[:matched + 1])
        out["unicorn"] = u
        out["note"] = "vor Unicorn-Ende (%s)" % classify_stop(u, thumb)
        return out
    if d is not None:
        out = _block_dev(d, addr, snap, thumb, ptrace)
        out["unicorn"] = u
        pregs = (ptrace[out["pi"] - 1][1] if out["pi"] > 0
                 else snap["regs"])
        uni_pages = fd.unicorn_pages(u["uc"], set(fd.pages_of(snap["regions"])))
        out["hint"] = lock_hint(out["prev"], thumb, uni_pages, {}, pregs)
        return out
    if port["status"].startswith(("LIMIT", "ABORTED")):
        return {"class": "port-limit (%s)" % port["status"], "unicorn": u,
                "snap": snap}
    if port["post"] is None:
        return {"class": "port-ohne-endzustand", "unicorn": u, "snap": snap}
    post = fd.read_snapshot(port["post"])
    pre_pages = fd.pages_of(snap["regions"])
    port_pages = fd.pages_of(post["regions"])
    uni_pages = fd.unicorn_pages(u["uc"], set(pre_pages) | set(port_pages))
    diffs, npages = fd.compare(u["regs"], u["flags"], uni_pages,
                               post["regs"], post["flags"], port_pages)
    if diffs:
        hint = lock_hint(ptrace[-1][0], thumb, uni_pages, port_pages,
                         post["regs"])
        return {"class": "abweichung:ende", "diffs": diffs, "pages": npages,
                "hint": hint, "unicorn": u, "snap": snap,
                "blocks": len(ptrace)}
    return {"class": "gleich", "insns": u["insns"], "blocks": len(ptrace),
            "unicorn": u, "snap": snap}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=".")
    ap.add_argument("--frames", type=int, default=1200)
    ap.add_argument("--reuse-perf", default=None)
    ap.add_argument("--perf-out", default="scratch/sweep/perf.txt")
    ap.add_argument("--out-dir", default="scratch/sweep/funcs")
    ap.add_argument("--report", default="scratch/sweep/report.md")
    ap.add_argument("--rare", type=int, default=8)
    ap.add_argument("--max-funcs", type=int, default=40)
    ap.add_argument("--budget", type=float, default=2700.0,
                    help="Gesamtzeit in Sekunden")
    ap.add_argument("--per-func", type=float, default=60.0)
    ap.add_argument("--only", default=None,
                    help="Adressen (Komma) statt Perfmap-Priorisierung")
    ap.add_argument("--limit-blocks", type=int, default=2000000)
    ap.add_argument("--unicorn-limit", type=int, default=3000000)
    ap.add_argument("--sort", choices=("plan", "addr"), default="plan",
                    help="plan = Prioritaet, addr = aufsteigend (Sweep-Fortsetzung)")
    a = ap.parse_args(argv)
    root = os.path.abspath(a.root)
    fd.ROOT = root
    fd.OUT = os.path.join(root, "out", "cosim")
    os.makedirs(os.path.join(root, a.out_dir), exist_ok=True)

    if a.only:
        plan = [(int(x, 0), -1, 0, "manuell")
                for x in a.only.split(",") if x.strip()]
        main_t = None
    else:
        perf = a.reuse_perf or os.path.join(root, a.perf_out)
        if not a.reuse_perf:
            print("Perfmap-Lauf (%d Frames) ..." % a.frames, flush=True)
            perfmap_run(root, a.frames, perf)
        rows, totals = parse_perfmap(perf)
        entries, empty = parse_entries(root)
        plan, main_t = build_plan(rows, totals, entries, empty, a.rare)
        if a.sort == "addr":
            plan.sort(key=lambda x: x[0])
        print("Perfmap: %d Basispc, %d Funktions-Einstiege, %d ausgefuehrt; "
              "Hauptthread=%s" % (len(rows), len(entries),
                                  len({p for p, _ in rows} & set(entries)),
                                  main_t), flush=True)
        print("Plan: %d selten/haupt, %d heiss/haupt, %d selten/neben, "
              "%d heiss/neben (Cap %d)"
              % (sum(1 for p in plan if p[3] == "selten/haupt"),
                 sum(1 for p in plan if p[3] == "heiss/haupt"),
                 sum(1 for p in plan if p[3] == "selten/neben"),
                 sum(1 for p in plan if p[3] == "heiss/neben"),
                 a.max_funcs), flush=True)

    report = []
    counts = {}
    t_start = time.time()
    tested = 0
    for addr, thread, count, group in plan:
        if tested >= a.max_funcs:
            break
        if time.time() - t_start > a.budget:
            report.append("- Budget erreicht: Sweep vorzeitig beendet.")
            break
        snap_path = os.path.join(root, a.out_dir, "snap_%08X.bin" % addr)
        sp, dt = snapshot(root, addr, snap_path, min(a.per_func, 45.0))
        if sp is None:
            line = ("| 0x%08X | %s | %s | - | snapshot-fehlend (%.1fs) |"
                    % (addr, group, thread, dt))
            report.append(line)
            counts["snapshot-fehlend"] = counts.get("snapshot-fehlend", 0) + 1
            print(line, flush=True)
            continue
        try:
            r = compare_func(addr, sp, a.limit_blocks, a.unicorn_limit,
                             timeout=a.per_func)
        except Exception as e:
            r = {"class": "werkzeug-fehler: %s" % e}
        tested += 1
        cls = r["class"]
        base = cls.split(" (")[0].split(":")[0]
        counts[base] = counts.get(base, 0) + 1
        extra = ""
        if cls == "abweichung:block":
            extra = ("%s an 0x%08X (Vorgaenger 0x%08X '%s')"
                     % (r.get("kind"), r["pc"], r["prev"], r["prev_insn"]))
            if r.get("reg") is not None:
                extra += " r%d unicorn=0x%08X port=0x%08X" % (
                    r["reg"], r["unicorn_wert"], r["port_wert"])
        elif cls == "abweichung:ende":
            extra = "%d Speicherseiten; %s" % (r["pages"], r["diffs"][:4])
        if r.get("hint"):
            extra += " HINWEIS: %s" % r["hint"]
        if r.get("note"):
            extra += " (%s)" % r["note"]
        line = ("| 0x%08X | %s | %s | %s | %s |"
                % (addr, group, thread, cls, extra))
        report.append(line)
        print(line, flush=True)
        if cls.startswith(("abweichung", "port-limit", "werkzeug")):
            with open(os.path.join(root, a.out_dir, "fail_%08X.txt" % addr),
                      "w", encoding="utf-8") as f:
                f.write("addr=0x%08X group=%s thread=%s\n%s\n"
                        % (addr, group, thread, extra))
                for d in r.get("diffs") or []:
                    f.write("  %s\n" % d)

    os.makedirs(os.path.dirname(os.path.abspath(
        os.path.join(root, a.report))), exist_ok=True)
    with open(os.path.join(root, a.report), "w", encoding="utf-8") as f:
        f.write("# Sweep-Bericht\n\n")
        f.write("Funktionen geprueft: %d in %.0f s\n\n"
                % (tested, time.time() - t_start))
        f.write("| Einstieg | Gruppe | Thread | Ergebnis | Detail |\n")
        f.write("|---|---|---|---|---|\n")
        for line in report:
            f.write(line + "\n")
        f.write("\n## Klassen\n\n")
        for k, v in sorted(counts.items()):
            f.write("- %s: %d\n" % (k, v))
    print("\nKlassen: %s" % counts, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
