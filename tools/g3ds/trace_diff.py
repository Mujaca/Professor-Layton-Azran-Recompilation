"""trace-diff: Azahar-Referenzlog vs. Port-Log kanonisch vergleichen."""
from __future__ import annotations

import difflib
import re
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple

from . import common


@dataclass
class Event:
    index: int
    key: str
    display: str
    service: str = ""
    cmd: Optional[int] = None
    params: Tuple[str, ...] = ()
    detail: str = ""
    line_no: int = 0
    raw: str = ""


@dataclass
class Trace:
    path: Path
    kind: str
    events: List[Event]
    services: List[str]

    @property
    def service_set(self) -> set:
        return {s.lower() for s in self.services}


_TS_RE = re.compile(r"^\[\s*([0-9]+\.[0-9]+)\]\s*")
_SRV_REF_RE = re.compile(r"GetServiceHandle:\d+:\s*called service=(\S+)")
_SVC_REF_RE = re.compile(r"^Service\.([A-Za-z0-9_]+)\s+<[^>]*>\s+(.*)$")
_METHOD_RE = re.compile(r"([A-Za-z0-9_]+)::([A-Za-z0-9_]+):\d+:")
_PATH_RE = re.compile(r"path=(?:\[Char:\s*)?([^\]\s]+)")

_PORT_SRV_RE = re.compile(r'^\[srvname\]\s+"([^"]+)"')
_PORT_IPC_RE = re.compile(
    r"^\[ipc\]\s+th=\S+\s+blk=\S+\s+(\S+)\s+cmd=0x([0-9A-Fa-f]{4})\s+"
    r"n=(\d+)\s+t=(\d+)\s+h=\S+(.*)$")
_PORT_FS_RE = re.compile(r'^\[fs\]\s+(\w+)\s+"([^"]*)"')
_PORT_GSP_RE = re.compile(r"^\[gsp\]\s+(\w+)(.*)$")
_PORT_SRVCMD_RE = re.compile(r"^\[srv\]\s+(.+)$")
_PNUM_RE = re.compile(r"\bp(\d+)=([0-9A-Fa-f]{8})")

_COMPONENT_MAP = {
    "srv": "srv", "fs": "fs", "gsp": "gsp", "apt": "apt", "boss": "boss",
    "dsp": "dsp", "cfg": "cfg", "ptm": "ptm", "hid": "hid", "ndm": "ndm",
    "y2r": "y2r", "am": "am", "cecd": "cecd", "news": "news", "nwm": "nwm",
    "frd": "frd", "soc": "soc", "ir": "ir", "mic": "mic", "cam": "cam",
    "ac": "ac", "ldr": "ldr", "mvd": "mvd", "nfc": "nfc", "pxi": "pxi",
    "dlp": "dlp", "ssl": "ssl", "http": "http",
}

_BASE_SERVICE = {
    "srv:": "srv", "fs:user": "fs", "fs:ldr": "fs", "file": "fs",
    "gsp::gpu": "gsp", "gsp::lcd": "gsp", "apt:u": "apt", "boss:u": "boss",
    "dsp::dsp": "dsp", "cfg:u": "cfg", "ptm:u": "ptm", "hid:user": "hid",
    "ndm:u": "ndm", "y2r:u": "y2r", "am:u": "am", "cecd:u": "cecd",
    "news:u": "news", "nwm::uds": "nwm", "frd:u": "frd", "soc:u": "soc",
    "ir:user": "ir", "mic:u": "mic", "cam:u": "cam", "ac:u": "ac",
}

# Verifizierte Cmd-IDs -> Methodenname. Nur fuer den Methodenvergleich in
# --kind ipc; unbekannte IDs bleiben als cmd=0xXXXX sichtbar.
_CMD_MAP = {
    ("fs:USER", 0x0802): "openfile",
    ("fs:USER", 0x0803): "openfiledirectly",
    ("fs:USER", 0x0808): "createfile",
    ("fs:USER", 0x080C): "openarchive",
    ("fs:USER", 0x080D): "controlarchive",
    ("fs:USER", 0x080E): "closearchive",
    ("fs:USER", 0x0812): "getfreebytes",
    ("file", 0x0802): "read",
    ("file", 0x0803): "write",
    ("file", 0x0804): "getsize",
    ("file", 0x0805): "setsize",
    ("file", 0x0808): "close",
    ("file", 0x0809): "flush",
    ("gsp::Gpu", 0x0001): "writehwregs",
    ("gsp::Gpu", 0x0004): "readhwregs",
    ("gsp::Gpu", 0x0005): "setbufferswap",
    ("gsp::Gpu", 0x0006): "setcommandlist",
    ("gsp::Gpu", 0x000C): "triggercmdreqqueue",
    ("gsp::Gpu", 0x0010): "setaxiconfigqosmode",
    ("gsp::Gpu", 0x0013): "registerinterruptrelayqueue",
    ("gsp::Gpu", 0x0016): "acquiregpuright",
    ("gsp::Gpu", 0x001E): "setinternalpriorities",
    ("APT:U", 0x0001): "getlockhandle",
    ("APT:U", 0x0002): "initialize",
    ("APT:U", 0x0003): "enable",
    ("APT:U", 0x0004): "finalize",
    ("APT:U", 0x0005): "getappletmaninfo",
    ("APT:U", 0x0009): "notifytowait",
    ("APT:U", 0x000D): "receiveparameter",
    ("APT:U", 0x000E): "glanceparameter",
    ("APT:U", 0x004B): "appletutility",
    ("APT:U", 0x004F): "setappcputimelimit",
    ("APT:U", 0x0050): "getappcputimelimit",
    ("dsp::DSP", 0x000D): "writeprocesspipe",
    ("dsp::DSP", 0x000E): "readpipe",
    ("dsp::DSP", 0x0010): "readpipeifpossible",
    ("dsp::DSP", 0x0011): "loadcomponent",
    ("dsp::DSP", 0x0012): "unloadcomponent",
    ("dsp::DSP", 0x0016): "getsemaphoreeventhandle",
    ("dsp::DSP", 0x0007): "setsemaphore",
    ("dsp::DSP", 0x0017): "setsemaphoremask",
    ("cfg:u", 0x0001): "getconfig",
    ("cfg:u", 0x0002): "getregion",
    ("cfg:u", 0x0003): "gettransferableid",
    ("cfg:u", 0x0005): "getsystemmodel",
    ("ptm:u", 0x000B): "getstephistory",
    ("ptm:u", 0x000C): "gettotalstepcount",
    ("ndm:u", 0x0006): "suspenddaemons",
    ("ndm:u", 0x0007): "resumedaemons",
    ("ndm:u", 0x0014): "overridedefaultdaemons",
    ("ndm:u", 0x0015): "resetdefaultdaemons",
}


def _base_service(name: str) -> str:
    return _BASE_SERVICE.get(name.lower(), name.lower())


def _cmd_method(service: str, cmd: int) -> Optional[str]:
    return _CMD_MAP.get((service, cmd))


def _norm_params(text: str) -> Tuple[str, ...]:
    return tuple("p%s=%s" % (i, v.upper())
                 for i, v in _PNUM_RE.findall(text or ""))


def parse_reference(path: Path) -> Trace:
    common.check_file(path, "Referenzlog",
                      "Azahar-Debuglog erwartet (reports/azahar_trace.txt)")
    events: List[Event] = []
    services: List[str] = []
    idx = 0
    for line_no, line in enumerate(
            common.read_text_auto(path).splitlines(), 1):
            # Azahar stellt jedem Ereignis "[   1.467578] " voran. Ohne
            # diesen Schritt matcht nur _SRV_REF_RE (search); alle
            # Service-Methodenzeilen gingen verloren.
            line = _TS_RE.sub("", line, count=1)
            m = _SRV_REF_RE.search(line)
            if m:
                name = m.group(1)
                if name.lower() not in [s.lower() for s in services]:
                    services.append(name)
                events.append(Event(idx, "srv.getservicehandle(%s)" % name.lower(),
                                    "srv:GetServiceHandle(%s)" % name,
                                    "srv", detail=name, line_no=line_no,
                                    raw=line))
                idx += 1
                continue
            m = _SVC_REF_RE.match(line)
            if not m:
                continue
            component = m.group(1).lower()
            rest = m.group(2)
            methods = _METHOD_RE.findall(rest)
            if not methods:
                continue
            _cls, method = methods[-1]
            service = _COMPONENT_MAP.get(component, component)
            if service == "srv" and method == "GetServiceHandle":
                continue
            msg = rest.split(": ", 1)[1] if ": " in rest else ""
            pm = _PATH_RE.search(msg)
            detail = ("path=" + pm.group(1)) if pm else msg.strip()
            if service.lower() not in [s.lower() for s in services]:
                services.append(service)
            events.append(Event(idx, "%s.%s" % (service, method.lower()),
                                "%s.%s%s" % (service, method,
                                             (" " + detail) if detail else ""),
                                service, detail=detail, line_no=line_no, raw=line))
            idx += 1
    if not events:
        common.fail("keine Service-Ereignisse im Referenzlog: %s" % path)
    return Trace(path, "azahar", events, services)


def parse_port(path: Path) -> Trace:
    common.check_file(path, "Port-Log",
                      "mit HLE_SRV_NAMES=1 erzeugen (guest-run)")
    events: List[Event] = []
    services: List[str] = []
    idx = 0

    def add_service(name: str):
        if name.lower() not in [s.lower() for s in services]:
            services.append(name)

    for line_no, line in enumerate(
            common.read_text_auto(path).splitlines(), 1):
            m = _PORT_SRV_RE.match(line)
            if m:
                name = m.group(1)
                add_service(name)
                events.append(Event(idx, "srv.getservicehandle(%s)" % name.lower(),
                                    "srv:GetServiceHandle(%s)" % name,
                                    "srv", detail=name, line_no=line_no,
                                    raw=line))
                idx += 1
                continue
            m = _PORT_IPC_RE.match(line)
            if m:
                name, cmd, _normal, _trans, rest = m.groups()
                add_service(name)
                cmd_i = int(cmd, 16)
                base = _base_service(name)
                method = _cmd_method(name, cmd_i)
                key = ("%s.%s" % (base, method)) if method else \
                    ("%s.cmd=0x%s" % (base, cmd.upper()))
                display = "%s cmd=0x%s%s" % (
                    name, cmd.upper(), (" (%s)" % method) if method else "")
                events.append(Event(idx, key, display, name, cmd=cmd_i,
                                    params=_norm_params(rest),
                                    line_no=line_no, raw=line))
                idx += 1
                continue
            m = _PORT_FS_RE.match(line)
            if m:
                method, arg = m.groups()
                add_service("fs")
                events.append(Event(idx, "fs.%s" % method.lower(),
                                    "fs.%s%s" % (method, (" " + arg) if arg else ""),
                                    "fs", detail=("path=" + arg) if arg else "",
                                    line_no=line_no, raw=line))
                idx += 1
                continue
            m = _PORT_GSP_RE.match(line)
            if m:
                method, rest = m.groups()
                add_service("gsp")
                events.append(Event(idx, "gsp.%s" % method.lower(),
                                    "gsp.%s%s" % (method, rest.rstrip()),
                                    "gsp", detail=rest.strip(),
                                    line_no=line_no, raw=line))
                idx += 1
                continue
            m = _PORT_SRVCMD_RE.match(line)
            if m:
                add_service("srv")
                events.append(Event(idx, "srv.%s" % m.group(1).split()[0].lower(),
                                    "srv.%s" % m.group(1), "srv",
                                    detail=m.group(1), line_no=line_no, raw=line))
                idx += 1
    if not events:
        common.fail("keine [srvname]/[ipc]-Ereignisse im Port-Log: %s  "
                    "(HLE_SRV_NAMES=1 setzen)" % path)
    return Trace(path, "port", events, services)


def sniff(path: Path) -> str:
    try:
        head = path.read_text(encoding="utf-8", errors="replace")[:20000]
    except OSError:
        common.fail("Log nicht lesbar: %s" % path)
    if "[srvname]" in head or "[ipc]" in head:
        return "port"
    if "Service." in head or re.search(r"^\[\s*[0-9]+\.[0-9]+\]", head, re.M):
        return "azahar"
    common.fail("Logformat unbekannt: %s (weder Azahar- noch Port-Log)" % path)


def compare(ref: Trace, port: Trace, kind: str = "srv") -> dict:
    if kind == "srv":
        ref_events = [e for e in ref.events if e.key.startswith("srv.getservicehandle(")]
        port_events = [e for e in port.events if e.key.startswith("srv.getservicehandle(")]
        ref_services: List[str] = []
        for e in ref_events:
            if e.detail.lower() not in [x.lower() for x in ref_services]:
                ref_services.append(e.detail)
        port_services: List[str] = []
        for e in port_events:
            if e.detail.lower() not in [x.lower() for x in port_services]:
                port_services.append(e.detail)
        port_present = port.service_set
        missing = [s for s in ref_services if s.lower() not in port_present]
        ref_set = {s.lower() for s in ref.services}
        extra = [s for s in port_services if s.lower() not in ref_set]
        base = {"ref_services": ref_services, "port_services": port_services,
                "missing": missing, "extra": extra,
                "ref_event": None, "port_event": None, "order": None}
        if missing:
            first = missing[0]
            pos = [s.lower() for s in ref_services].index(first.lower())
            result = dict(base)
            result.update({"mode": "missing-service", "first": first,
                           "ref_pos": pos})
            return result
        sm = difflib.SequenceMatcher(
            a=[s.lower() for s in ref_services],
            b=[s.lower() for s in port_services], autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == "equal":
                continue
            if tag in ("delete", "replace"):
                result = dict(base)
                result.update({"mode": "order", "first": ref_services[i1],
                               "ref_pos": i1,
                               "order": (tag, i1, i2, j1, j2)})
                return result
            if tag == "insert":
                result = dict(base)
                result.update({"mode": "extra", "first": port_services[j1],
                               "ref_pos": i1,
                               "order": (tag, i1, i2, j1, j2)})
                return result
        result = dict(base)
        result.update({"mode": "equal", "first": None, "ref_pos": None})
        return result

    if kind == "ipc":
        ref_events = [e for e in ref.events
                      if not e.key.startswith("srv.")]
        port_events = [e for e in port.events
                       if not e.key.startswith("srv.")]
    else:
        ref_events = list(ref.events)
        port_events = list(port.events)
    ref_keys = [e.key for e in ref_events]
    port_keys = [e.key for e in port_events]
    port_set = set(port_keys)
    sm = difflib.SequenceMatcher(a=ref_keys, b=port_keys, autojunk=False)
    base = {"ref_services": ref.services, "port_services": port.services,
            "missing": [], "extra": [], "ref_event": None,
            "port_event": None, "order": None}
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            continue
        if tag in ("delete", "replace"):
            ev = ref_events[i1]
            result = dict(base)
            result.update({"mode": "order" if ev.key in port_set else "missing-event",
                           "first": ev.display, "ref_event": ev,
                           "port_event": port_events[j1] if j1 < len(port_events) else None,
                           "ref_pos": i1, "order": (tag, i1, i2, j1, j2)})
            return result
        if tag == "insert":
            ev = port_events[j1]
            result = dict(base)
            result.update({"mode": "extra-event", "first": ev.display,
                           "port_event": ev,
                           "ref_event": ref_events[i1] if i1 < len(ref_events) else None,
                           "ref_pos": i1, "order": (tag, i1, i2, j1, j2)})
            return result
    result = dict(base)
    result.update({"mode": "equal", "first": None, "ref_pos": None})
    return result


def render(result: dict, ref: Trace, port: Trace) -> List[str]:
    lines = ["trace-diff: Referenz %d Ereignisse / %d Dienste, "
             "Port %d Ereignisse / %d Dienste"
             % (len(ref.events), len(ref.services),
                len(port.events), len(port.services))]
    mode = result["mode"]
    if mode == "equal":
        lines.append("Erste Divergenz: keine (kanonische Sequenzen gleich)")
        return lines
    first = result["first"]
    if mode == "missing-service":
        lines.append("Erste Divergenz: Referenz: srv:GetServiceHandle(%s) "
                     "-> Port: fehlt" % first)
        lines.append("  Referenz-Position: %d/%d"
                     % (result["ref_pos"] + 1, len(result["ref_services"])))
        if result["missing"]:
            lines.append("  Fehlende Dienste: " + ", ".join(result["missing"]))
    elif mode == "order":
        lines.append("Erste Divergenz (Reihenfolge): %s" % first)
    elif mode == "extra":
        lines.append("Erste Divergenz: Port: %s -> Referenz: fehlt" % first)
    elif mode == "missing-event":
        lines.append("Erste Divergenz: Referenz: %s -> Port: fehlt" % first)
        if result.get("port_event") is not None:
            lines.append("  Port hat dort: %s" % result["port_event"].display)
    elif mode == "extra-event":
        lines.append("Erste Divergenz: Port: %s -> Referenz: fehlt" % first)
    else:
        lines.append("Erste Divergenz: %s" % first)
    if result["extra"]:
        lines.append("  Nur im Port: " + ", ".join(result["extra"]))
    return lines


def render_table(ref: Trace, port: Trace) -> List[str]:
    """Erwartete vs. beobachtete Dienste (erste Acquisitions/Nennungen)."""
    def firsts(tr):
        out = []
        for e in tr.events:
            if not e.key.startswith("srv.getservicehandle("):
                continue
            if e.detail.lower() not in [x.lower() for x in out]:
                out.append(e.detail)
        return out

    def first_mentions(tr):
        out = {}
        for e in tr.events:
            if e.key.startswith("srv.getservicehandle("):
                name = e.detail
            elif e.service:
                name = e.service
            else:
                continue
            if name.lower() not in [x.lower() for x in out]:
                out[name] = "%s (via %s)" % (name, e.key.split(".")[0])
        return out

    a = firsts(ref)
    b = first_mentions(port)
    b_lower = {k.lower(): v for k, v in b.items()}
    lines = ["Dienst-Praesenz (Referenz-Reihenfolge):",
             "  #   Referenz             Port"]
    for i, name in enumerate(a):
        if name.lower() in b_lower:
            right = b_lower[name.lower()]
        else:
            right = "-"
        mark = " " if right != "-" else "!"
        lines.append("%s %2d  %-20s %s" % (mark, i, name, right))
    return lines
