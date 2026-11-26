"""Baut und startet tests/lua_host_selftest.c (Lua-Mods, Stufe B).

  python tests/lua_host_test.py

Nur synthetische Skripte und ein falscher Gastspeicher (kein Spiel).
Geprueft:

* Sandbox: io/os/debug/package/dofile/loadfile/string.dump fehlen,
  require laedt nur scripts/<name>.lua desselben Mods (keine C-Module,
  keine Pfade nach aussen), load nimmt keine Binaer-Chunks,
  math.random fest geseedet (zwei Laeufe gleich), eigene Umgebung je Mod,
* API auf dem falschen Speicher: mem.read_*/write_* (Vorzeichen, f32,
  ungueltige Adresse = Lua-Fehler), input.get/input.set (nur dieser VBlank,
  hoehere Prioritaet gewinnt), host.mouse/key/set_cursor, guest.*, log,
* Fehler-Isolation: Fehler mit Traceback geloggt, nur dieser Mod aus,
  Syntaxfehler in main.lua,
* Instruktionsbudget: Endlosschleife (auch in pcall/xpcall/coroutine) wird
  abgebrochen, der Mod deaktiviert, andere laufen weiter.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import build as portbuild  # noqa: E402

EXE = os.path.join(ROOT, "tools", "out", "lua_host_selftest.exe")
WORK = os.path.join(ROOT, "tools", "out", "lua_host_test")

FAILS = []


def check(cond, msg):
    print(("[OK]   " if cond else "[FAIL] ") + msg)
    if not cond:
        FAILS.append(msg)


def build():
    zig = portbuild.zig()
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    cmd = [zig, "cc", "-std=c11", "-O1",
           "-I", os.path.join(ROOT, "runtime", "modding"),
           "-I", os.path.join(ROOT, "runtime", "platform")] + portbuild.lua_flags() + [
           os.path.join(ROOT, "runtime", "modding", "lua_host.c"),
           os.path.join(ROOT, "tests", "lua_host_selftest.c")] + portbuild.lua_sources() + ["-o", EXE]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print("BUILD-FEHLER:\n" + p.stderr[-3000:])
        sys.exit(1)


def make_mods(name, mods):
    """mods: {ordner: (priority, {relpfad: text})}"""
    root = os.path.join(WORK, name)
    shutil.rmtree(root, ignore_errors=True)
    for folder, (prio, files) in mods.items():
        d = os.path.join(root, folder)
        os.makedirs(d, exist_ok=True)
        if prio is not None:
            with open(os.path.join(d, "mod.ini"), "w") as f:
                f.write("name = %s\npriority = %d\n" % (folder, prio))
        for rel, text in files.items():
            p = os.path.join(d, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "w", encoding="utf-8") as f:
                f.write(text)
    return root


def run(root, vblanks, budget=None, timeout=60):
    args = [EXE, root, str(vblanks)] + ([str(budget)] if budget else [])
    p = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout


def lines(out, prefix):
    return [ln for ln in out.splitlines() if ln.startswith(prefix)]


SANDBOX = r'''
local absent = {}
for _, n in ipairs({"io", "os", "debug", "package", "dofile", "loadfile", "loadstring",
                    "warn"}) do
  if _G[n] ~= nil then absent[#absent + 1] = n end
end
if string.dump ~= nil then absent[#absent + 1] = "string.dump" end
log("absent-violations=" .. #absent .. " " .. table.concat(absent, ","))
log("have", type(string.format), type(table.insert), type(math.floor), type(utf8.char),
    type(coroutine.wrap), type(collectgarbage), type(pcall))
local ok1, e1 = pcall(require, "socket")
log("req-socket", ok1, (tostring(e1):find("nicht gefunden") ~= nil))
local ok2 = pcall(require, "..\\..\\x")
local ok3 = pcall(require, "a/b")
log("req-path", ok2, ok3)
local bin = "\27Lua\84\0\25\147\13\10\26\10"
local f, err = load(bin)
log("load-bin", f == nil, (tostring(err):find("binary") ~= nil) or (tostring(err):find("attempt to load") ~= nil))
local g = load("return 1 + 2")
log("load-text", g and g())
log("rand", math.random(1, 1000000), math.random(1, 1000000))
math.randomseed()
log("rand0", math.random(1, 1000000))
local helper = require("lib.helper")
log("helper", helper.twice(21), require("lib.helper") == helper)
shared_global = "A"
on_load(function() log("onload", guest.frame(), input.get().a) end)
'''

HELPER = r'''
local M = {}
function M.twice(x) return 2 * x end
return M
'''

OTHER_ENV = r'''
on_load(function() log("other-sees", tostring(shared_global), mod.name) end)
'''

API = r'''
log("u8", mem.read_u8(0x100003), "u16", mem.read_u16(0x100002), "u32", string.format("%08X", mem.read_u32(0x100000)))
log("s8", mem.read_s8(0x1000FF), "s16", mem.read_s16(0x1000FE), "s32", mem.read_s32(0x1000FC))
mem.write_u32(0x100010, 0xDEADBEEF)
mem.write_u16(0x100014, -2)
mem.write_u8(0x100016, 0x1FF)
log("rw", string.format("%08X %04X %02X", mem.read_u32(0x100010), mem.read_u16(0x100014), mem.read_u8(0x100016)))
mem.write_f32(0x100020, 1.5)
log("f32", mem.read_f32(0x100020), string.format("%08X", mem.read_u32(0x100020)))
local ok, e = pcall(mem.read_u32, 0x200000)
log("bad-read", ok, (tostring(e):find("ungueltig") ~= nil))
ok, e = pcall(mem.write_u8, 0x0FFF, 1)
log("bad-write", ok, (tostring(e):find("ungueltig") ~= nil))
ok, e = pcall(mem.read_u8, -1)
log("bad-neg", ok)
ok, e = pcall(input.set, {a = true})
log("set-outside", ok)
local m = host.mouse()
log("mouse", m.x, m.y, m.inside_touch, m.down, m.over_window)
log("keys", host.key("F5"), host.key("A"), pcall(host.key, "NOPE"))
host.set_cursor("crosshair")
log("cursor-bad", pcall(host.set_cursor, "fancy"))
on_frame(function(frame)
  local i = input.get()
  log("frame", frame, guest.frame(), guest.vblank(), guest.time_ns(), i.a, i.touch_x)
  if guest.vblank() == 2 then input.set{a = true, touch = true, touch_x = 400, touch_y = 7} end
  if guest.vblank() == 3 then input.set{start = true, circle_x = -500} ; log("after-set", input.get().start, input.get().circle_x) end
  if guest.vblank() == 4 then log("set-badfield", pcall(input.set, {zzz = true})) end
  if guest.vblank() == 5 then mem.write_u8(0x100000, 0x77) end
end)
'''

PRIO_LO = r'''
on_frame(function() if guest.vblank() == 1 then input.set{b = true, a = false} end end)
'''
PRIO_HI = r'''
on_frame(function() if guest.vblank() == 1 then input.set{b = false} end end)
'''

BAD = r'''
on_frame(function(frame)
  if guest.vblank() == 3 then local t = nil; return t.x end
  log("bad-alive", guest.vblank())
end)
'''
GOOD = r'''
on_frame(function() log("good-alive", guest.vblank()) end)
'''
SYNTAX = "this is not lua ((("

LOOP_PLAIN = r'''
on_frame(function() if guest.vblank() == 1 then while true do end end end)
'''
LOOP_PCALL = r'''
on_frame(function()
  if guest.vblank() == 1 then
    while true do pcall(function() while true do end end) end
  end
end)
'''
LOOP_XPCALL = r'''
on_frame(function()
  if guest.vblank() == 1 then
    while true do xpcall(function() while true do end end, function(e) return e end) end
  end
end)
'''
LOOP_CORO = r'''
on_frame(function()
  if guest.vblank() == 1 then
    while true do coroutine.resume(coroutine.create(function() while true do end end)) end
  end
end)
'''
LOOP_LOAD = "while true do end\n"


def main():
    build()
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)

    # Ohne Lua-Mods inaktiv
    root = make_mods("leer", {"nurdaten": (1, {"romfs/x.bin": "x"})})
    rc, out = run(root, 2)
    check(rc == 0 and "[test] init=0 active=0" in out and "mask=00000" in out,
          "ohne scripts/main.lua: Lua inaktiv, keine Ueberschreibung")

    # Sandbox
    root = make_mods("sandbox", {
        "sb": (1, {"scripts/main.lua": SANDBOX, "scripts/lib/helper.lua": HELPER}),
        "zz-other": (2, {"scripts/main.lua": OTHER_ENV}),
    })
    rc, out = run(root, 1)
    print(out, end="")
    log = "\n".join(lines(out, "[mod:sb]"))
    check(rc == 0 and "absent-violations=0" in log, "Sandbox: io/os/debug/package/dofile/loadfile/string.dump fehlen")
    check("have function function function function function function function" in log,
          "Sandbox: string/table/math/utf8/coroutine/collectgarbage/pcall vorhanden")
    check("req-socket false true" in log and "req-path false false" in log,
          "require: kein C-Modul, keine Pfade ausserhalb scripts/")
    check("load-bin true true" in log and "load-text 3" in log, "load: Binaer-Chunk abgelehnt, Text geht")
    check("helper 42 true" in log, "require: Modul aus scripts/lib/ desselben Mods, gecacht")
    check("onload 0 false" in log, "on_load nach dem Hauptchunk")
    check("[mod:zz-other] other-sees nil zz-other" in out, "eigene Umgebung je Mod (globale Variablen getrennt)")
    rnd = [ln for ln in lines(out, "[mod:sb] rand")]
    rc2, out2 = run(root, 1)
    check(len(rnd) == 2 and rnd == lines(out2, "[mod:sb] rand"), "math.random fest geseedet (zwei Laeufe gleich)")

    # API
    root = make_mods("api", {"api": (0, {"scripts/main.lua": API})})
    rc, out = run(root, 7)
    print(out, end="")
    log = "\n".join(lines(out, "[mod:api]"))
    check("u8 3 u16 770 u32 03020100" in log, "mem.read_u8/u16/u32 little-endian")
    check("s8 -1 s16 -2 s32 -66052" in log, "mem.read_s8/s16/s32 mit Vorzeichen")
    check("rw DEADBEEF FFFE FF" in log, "mem.write_u32/u16/u8 (Werte maskiert)")
    check("f32 1.5 3FC00000" in log, "mem.write_f32/read_f32")
    check("bad-read false true" in log and "bad-write false true" in log and "bad-neg false" in log,
          "ungueltige Adresse = Lua-Fehler")
    check("set-outside false" in log, "input.set ausserhalb on_frame = Fehler")
    check("mouse 12 34 true false true" in log, "host.mouse")
    check("keys true false false" in log, "host.key (unbekannter Name = Fehler)")
    check("[test] cursor=2" in out and "cursor-bad false" in log, "host.set_cursor")
    fr = [ln.split()[2:] for ln in lines(out, "[mod:api] frame")]
    ok = len(fr) == 7 and all(fr[k][0] == str(k // 2) and fr[k][1] == str(k // 2) and fr[k][2] == str(k + 1)
                               and fr[k][3] == str(k * 16666667) for k in range(7))
    check(ok, "on_frame(frame), guest.frame/vblank/time_ns je VBlank")
    check(all(fr[k][4] == ("true" if k & 1 else "false") and fr[k][5] == "100" for k in range(7)),
          "input.get liefert die Basis-Eingabe")
    vb = {int(ln.split()[1].split("=")[1]): ln for ln in lines(out, "[test] vbl=")}
    check("mask=00000" in vb[0] and "mask=00000" in vb[4], "ohne input.set keine Ueberschreibung")
    check("mask=1C001" in vb[1] and "a=1" in vb[1] and "touch=1,319,7" in vb[1],
          "input.set: Taste/Touch gesetzt, Koordinaten geklemmt")
    check("mask=01040" in vb[2] and "start=1" in vb[2] and "circle=-156,0" in vb[2] and
          "after-set true -156" in log, "input.set: Circle geklemmt, input.get sieht die Ueberschreibung")
    check("set-badfield false" in log, "input.set: unbekanntes Feld = Fehler")
    check("w0=03020177" in out, "mem.write_u8 im Frame-Callback")

    # Prioritaet
    root = make_mods("prio", {"lo": (1, {"scripts/main.lua": PRIO_LO}),
                              "hi": (9, {"scripts/main.lua": PRIO_HI})})
    rc, out = run(root, 2)
    vb = lines(out, "[test] vbl=0")
    check(vb and "mask=00003" in vb[0] and "a=0 b=0" in vb[0], "input.set: hoehere Prioritaet gewinnt je Feld")

    # Fehler-Isolation
    root = make_mods("fehler", {"bad": (0, {"scripts/main.lua": BAD}),
                                "good": (0, {"scripts/main.lua": GOOD}),
                                "syntax": (0, {"scripts/main.lua": SYNTAX})})
    rc, out = run(root, 6)
    print("\n".join(ln for ln in out.splitlines() if "Fehler" in ln or "deaktiviert" in ln or "traceback" in ln))
    check(rc == 0 and "[mod:bad] Fehler:" in out and "stack traceback" in out and
          "bad/scripts/main.lua:3" in out, "Laufzeitfehler mit Modname, Datei:Zeile und Traceback geloggt")
    check(lines(out, "[mod:bad] bad-alive") == ["[mod:bad] bad-alive 1", "[mod:bad] bad-alive 2"],
          "fehlerhafter Mod danach nicht mehr aufgerufen")
    check(len(lines(out, "[mod:good] good-alive")) == 6, "anderer Mod laeuft weiter")
    check("[mod:syntax] Fehler:" in out and "[mod:syntax] deaktiviert" in out, "Syntaxfehler in main.lua isoliert")

    # Instruktionsbudget
    for name, src in [("plain", LOOP_PLAIN), ("pcall", LOOP_PCALL), ("xpcall", LOOP_XPCALL),
                      ("coro", LOOP_CORO)]:
        root = make_mods("budget-" + name, {"loop": (0, {"scripts/main.lua": src}),
                                            "good": (0, {"scripts/main.lua": GOOD})})
        try:
            rc, out = run(root, 4, budget=2000000, timeout=60)
        except subprocess.TimeoutExpired:
            check(False, "Budget %s: Zeitueberschreitung" % name)
            continue
        check(rc == 0 and "Instruktionsbudget" in out and "[mod:loop] deaktiviert" in out and
              len(lines(out, "[mod:good] good-alive")) == 4,
              "Budget: Endlosschleife (%s) abgebrochen, anderer Mod laeuft weiter" % name)
    root = make_mods("budget-load", {"loop": (0, {"scripts/main.lua": LOOP_LOAD})})
    rc, out = run(root, 1, budget=2000000)
    check(rc == 0 and "Instruktionsbudget" in out, "Budget gilt auch im Hauptchunk")

    print("%d Fehler" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
