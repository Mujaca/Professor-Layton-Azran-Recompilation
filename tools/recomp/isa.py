"""ARM/Thumb-Decoding + Operanden-Helfer (capstone)."""
import re
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN
import capstone.arm as A

ARM_BASE = 0x100000

_CS = {
    "arm": Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN),
    "thumb": Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN),
}
for _c in _CS.values():
    _c.detail = True

REGNAMES = {
    "r0": 0, "r1": 1, "r2": 2, "r3": 3, "r4": 4, "r5": 5, "r6": 6, "r7": 7,
    "r8": 8, "r9": 9, "r10": 10, "r11": 11, "r12": 12,
    "sp": 13, "lr": 14, "pc": 15, "ip": 12, "fp": 11, "sl": 10, "sb": 9,
}
for _i in range(16):
    REGNAMES[f"r{_i}"] = _i


def decode(data: bytes, addr: int, thumb: bool = False):
    """Dekodiert genau eine Instruktion an addr (oder None)."""
    off = addr - ARM_BASE
    if off < 0 or off + 4 > len(data):
        return None
    cs = _CS["thumb" if thumb else "arm"]
    for ins in cs.disasm(data[off:off + 4], addr):
        return ins
    return None


def split_operands(text: str):
    """Splittet 'r0, [r1, #4]!' an Top-Level-Kommas (Klammern werden respektiert)."""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "[{(":
            depth += 1
        elif ch in "]})":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def reg_num(token: str):
    """'r3' -> 3, 's0' -> None. None bei Nicht-Kern-Register."""
    t = token.strip().lower()
    if t in REGNAMES:
        return REGNAMES[t]
    return None


def vfp_reg(token: str):
    """'s12' -> ('s', 12), 'd3' -> ('d', 3), sonst None."""
    m = re.fullmatch(r"\s*([sd])(\d+)\s*", token.strip().lower())
    if not m:
        return None
    return (m.group(1), int(m.group(2)))


def imm_value(token: str):
    """'#0x1f' -> 31, '#'-Praefix optional."""
    t = token.strip().lstrip("#").strip()
    neg = t.startswith("-")
    if neg:
        t = t[1:].strip()
    try:
        v = int(t, 0)
    except ValueError:
        return None
    return -v if neg else v


def reg_list(text: str):
    """'{r4, lr}' -> [4, 14]"""
    t = text.strip()
    if t.startswith("{") and t.endswith("}"):
        t = t[1:-1]
    regs = []
    for part in t.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:  # Bereich: r4-r7
            lo, hi = part.split("-", 1)
            a, b = reg_num(lo), reg_num(hi)
            if a is None or b is None:
                raise ValueError(f"kein Registerbereich: {part!r}")
            regs.extend(range(a, b + 1))
        else:
            n = reg_num(part)
            if n is None:
                raise ValueError(f"kein Register: {part!r}")
            regs.append(n)
    return regs


def is_pc(token: str) -> bool:
    return reg_num(token) == 15


def vfp_reg_list(text: str):
    """'{s0-s3}' -> [('s',0),('s',1),('s',2),('s',3)]"""
    t = text.strip()
    if t.startswith("{") and t.endswith("}"):
        t = t[1:-1]
    out = []
    for part in t.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            a, b = vfp_reg(lo), vfp_reg(hi)
            if a is None or b is None or a[0] != b[0]:
                raise ValueError(part)
            for i in range(a[1], b[1] + 1):
                out.append((a[0], i))
        else:
            r = vfp_reg(part)
            if r is None:
                raise ValueError(part)
            out.append(r)
    return out
