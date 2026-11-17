import io

# 1) Runtime: einfacher Host-Pool fuer SDK-Objekte
p = r"runtime\arm_cpu.h"
s = io.open(p, encoding="utf-8").read()
if "arm_pool_alloc" not in s:
    s = s.replace("void arm_mutex_bootstrap(ArmCpu *c, uint32_t addr);",
                  "void arm_mutex_bootstrap(ArmCpu *c, uint32_t addr);\n"
                  "void *arm_pool_alloc(unsigned size);")
io.open(p, "w", encoding="utf-8", newline="\n").write(s)

p = r"runtime\arm_cpu.c"
s = io.open(p, encoding="utf-8").read()
if "arm_pool_alloc" not in s:
    s = s.replace("/* Null-initialisierte BSS-Semaphore",
'''/* Einfacher Bump-Allocator fuer SDK-Objekte (Ersatz fuer leere SDK-Pools). */
void *arm_pool_alloc(unsigned size) {
    static unsigned char pool[1 << 18];
    static unsigned used = 0;
    size = (size + 15u) & ~15u;
    if (used + size > sizeof(pool)) return NULL;
    void *p = &pool[used];
    used += size;
    memset(p, 0, size);
    return p;
}

/* Null-initialisierte BSS-Semaphore''')
io.open(p, "w", encoding="utf-8", newline="\n").write(s)

# 2) Emitter: nach Aufruf von 0x287DD4 NULL-Fallback
p = r"tools\recomp\emit.py"
s = io.open(p, encoding="utf-8").read()
old = '''            nxt = pc + ins.size + (1 if self.thumb else 0)
            return ["c->r[14] = 0x%08Xu;" % u32(nxt),
                    "fn_%08X(c);" % u32(tgt),
                    "if (c->aborted) return;"]'''
new = '''            nxt = pc + ins.size + (1 if self.thumb else 0)
            lines = ["c->r[14] = 0x%08Xu;" % u32(nxt),
                     "fn_%08X(c);" % u32(tgt),
                     "if (c->aborted) return;"]
            if u32(tgt) == 0x00287DD4:
                lines.append("{ if (c->r[0] == 0) c->r[0] = "
                             "(uint32_t)(uintptr_t)arm_pool_alloc(0x100); }")
            return lines'''
if old in s:
    s = s.replace(old, new, 1)
    io.open(p, "w", encoding="utf-8", newline="\n").write(s)
    print("ok")
else:
    print("emit: Block nicht gefunden")
