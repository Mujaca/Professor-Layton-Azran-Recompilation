import io

p = r"tools\recomp\cli.py"
s = io.open(p, encoding="utf-8").read()
if "0x001079D0" not in s:
    s = s.replace('''            if u32(blk.addr) in (0x0012E1E4, 0x0012E1F0):''',
'''            if u32(blk.addr) == 0x001079D0:
                # SDK erwartet vom Kernel geschriebene Reply-Groesse 0x28
                out.append("  c->r[0] = 0x28u;")
            if u32(blk.addr) == 0x00107A30:
                out.append("  c->r[6] = 0u;")
            if u32(blk.addr) in (0x0012E1E4, 0x0012E1F0):''')
io.open(p, "w", encoding="utf-8", newline="\n").write(s)
print("ok")
