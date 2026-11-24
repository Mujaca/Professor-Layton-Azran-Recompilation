"""Selbst-Trace: recompilierten Code ausfuehren und den Trace auswerten.

  python tests/self_trace.py
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from recomp import cli, trace as tracemod  # noqa: E402

OUT = os.path.join(ROOT, "out", "selftrace")
GEN = os.path.join(OUT, "generated_trace.c")
TRACE = os.path.join(OUT, "run.ctrt")
HOST = os.path.join(OUT, "host.c")

HOST_SRC = """\
#include "arm_cpu.h"
#include "arm_trace.h"
#include <stdio.h>
#include <stdlib.h>

void fn_00100000(ArmCpu *c);

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "run.ctrt";
    const unsigned mem_size = 0x30000000u;
    unsigned char *mem = (unsigned char *)calloc(1, mem_size);
    if (!mem) { printf("kein Speicher\\n"); return 2; }

    ArmCpu c;
    arm_cpu_init(&c, mem, mem_size);
    c.r[13] = 0x08100000u;   /* Stack in FCRAM */

    arm_trace_open(path);
    fn_00100000(&c);
    arm_trace_close();

    printf("aborted=%d pc=0x%08X\\n", c.aborted, c.r[15]);
    free(mem);
    return 0;
}
"""


def main():
    os.makedirs(OUT, exist_ok=True)

    data = open(os.path.join(ROOT, "out", "exefs", "code_decomp.bin"), "rb").read()
    res = cli.scanmod.scan(data, [(0x100000, "entry", False)], follow_prologues=True)
    n_arm, n_thumb = cli.generate(res, GEN, trace=True)
    print("generiert: %d ARM, %d Thumb (mit Trace-Aufrufen)" % (n_arm, n_thumb))

    with open(HOST, "w", encoding="utf-8") as f:
        f.write(HOST_SRC)

    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), "zig.exe")
    runtime = os.path.join(ROOT, "runtime")
    exe = os.path.join(OUT, "selftrace.exe")
    cmd = [zig, "cc", "-std=c11", "-O1", "-I", os.path.join(runtime, "cpu"),
           HOST, GEN, os.path.join(runtime, "cpu", "arm_cpu.c"),
           os.path.join(runtime, "cpu", "arm_trace.c"), os.path.join(runtime, "workarounds.c"), "-o", exe]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print("COMPILE-FEHLER\n", p.stdout, p.stderr)
        return 1

    r = subprocess.run([exe, TRACE], capture_output=True, text=True)
    print("Lauf:", r.stdout.strip(), r.stderr.strip()[:200])

    st = tracemod.stats(TRACE)
    blocks = st.pop("blocks")
    print("\nTrace-Statistik:")
    print(json.dumps(st, indent=2)[:1200])
    print("\nTrace-Datei: %.1f KiB, %d Records" % (
        os.path.getsize(TRACE) / 1024.0, sum(st["records"].values())))

    cov = tracemod.coverage(TRACE, res)
    print("\nCoverage gegen Scanner:")
    print(json.dumps(cov, indent=2)[:600])
    return 0


if __name__ == "__main__":
    sys.exit(main())
