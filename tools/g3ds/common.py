"""Gemeinsame Helfer: Pfade, Zahlen, Ausgabe, Fehler."""
from __future__ import annotations

import re
import sys
from pathlib import Path

# tools/g3ds/common.py -> tools/g3ds -> tools -> Repo-Wurzel
ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
OUT = TOOLS / "out"
CODE_BIN = ROOT / "out" / "exefs" / "code_decomp.bin"
GEN_BOOT = ROOT / "build" / "gen_boot.c"
ARM_BASE = 0x00100000


def repo_path(*parts: str) -> Path:
    return ROOT.joinpath(*parts)


def ensure_out() -> Path:
    OUT.mkdir(parents=True, exist_ok=True)
    return OUT


def parse_int(text: str) -> int:
    """0x10, 10 oder reine Hex-Ziffern (z. B. 40B4C4) lesen."""
    s = str(text).strip().replace("_", "")
    if not s:
        raise ValueError("leere Zahl")
    if s.lower().startswith("0x"):
        return int(s, 16)
    if re.fullmatch(r"[0-9]+", s):
        return int(s, 10)
    if re.fullmatch(r"[0-9A-Fa-f]+", s):
        return int(s, 16)
    raise ValueError("keine Zahl: %r" % text)


def hexs(value: int, width: int = 8) -> str:
    return "0x%0*X" % (width, value & 0xFFFFFFFF)


def fail(message: str, code: int = 2):
    print("FEHLER: %s" % message, file=sys.stderr)
    raise SystemExit(code)


def warn(message: str) -> None:
    print("WARNUNG: %s" % message, file=sys.stderr)


def read_text_auto(path: Path) -> str:
    """Logtext lesen: UTF-8, UTF-16 (PowerShell-Redirect) oder cp1252."""
    raw = Path(path).read_bytes()
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        return raw.decode("utf-16", errors="replace")
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw.decode("utf-8-sig", errors="replace")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("cp1252", errors="replace")


def check_file(path: Path, what: str, hint: str = "") -> Path:
    if not path.is_file():
        msg = "%s fehlt: %s" % (what, path)
        if hint:
            msg += "  (%s)" % hint
        fail(msg)
    return path


def u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        return 0
    return int.from_bytes(data[offset:offset + 4], "little")


def s32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def align4(value: int) -> int:
    return value & ~3


def fmt_bytes(n: int) -> str:
    if n < 1024:
        return "%d B" % n
    if n < 1024 * 1024:
        return "%.1f KiB" % (n / 1024.0)
    return "%.1f MiB" % (n / (1024.0 * 1024.0))


class Table:
    """Kleiner Tabellen-Renderer mit Spaltenbreiten-Limit."""

    def __init__(self, columns, max_width=52):
        self.columns = list(columns)
        self.max_width = max_width
        self.rows = []

    def add(self, *cells):
        self.rows.append(["" if c is None else str(c) for c in cells])

    def render(self) -> str:
        if not self.rows:
            return "(keine Zeilen)"
        widths = [len(c) for c in self.columns]
        for row in self.rows:
            for i, cell in enumerate(row):
                if i < len(widths):
                    widths[i] = max(widths[i], len(cell))
        widths = [min(w, self.max_width) for w in widths]

        def cut(s, w):
            return s if len(s) <= w else s[:max(0, w - 1)] + "~"

        out = []
        out.append("  ".join(cut(c, w).ljust(w)
                             for c, w in zip(self.columns, widths)))
        out.append("  ".join("-" * w for w in widths))
        for row in self.rows:
            out.append("  ".join(cut(c, w).ljust(w)
                                 for c, w in zip(row, widths)))
        return "\n".join(out)


def print_summary(lines, max_lines: int = 20) -> None:
    """Gibt hoechstens max_lines Zeilen aus und weist auf --verbose hin."""
    body = list(lines)
    if len(body) > max_lines:
        body = body[:max_lines - 1] + ["... (Details mit --verbose)"]
    for line in body:
        print(line)


def env_dict(pairs) -> dict:
    out = {}
    for item in pairs or []:
        if "=" not in item:
            fail("--env erwartet NAME=WERT, bekam %r" % item)
        k, v = item.split("=", 1)
        out[k.strip()] = v
    return out


def git_commit() -> str:
    try:
        import subprocess
        p = subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(ROOT),
                           capture_output=True, text=True, timeout=5)
        if p.returncode == 0:
            return p.stdout.strip()
    except Exception:
        pass
    return "unbekannt"
