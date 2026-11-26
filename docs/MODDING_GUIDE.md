# Modding Guide

This guide is for people who want to **use or write mods** for the port.
The design notes and implementation details (German) are in
[`MODDING.md`](MODDING.md).

> **No game content in this repository.** Mods that replace text, graphics or
> sound necessarily contain material derived from the game. Build them from
> *your own* dump with `tools/modkit.py` and do not commit them to this
> repository. Share them only where you are allowed to.

## Quick start

1. Build the port as described in the [README](../README.md).
2. Create a folder `mods/` in the repository root (the folder you start
   `out\port.exe` from). Every subfolder of `mods/` is one mod.
3. Start the game with mods enabled:

   ```powershell
   .\out\port.exe --mods
   ```

   or enable them permanently in `out/port.ini`:

   ```ini
   [mods]
   enabled = 1
   # dir = mods
   ```

Mods are **off by default**. `--no-mods` overrides the ini, `--mods-dir
<folder>` loads mods from another folder. Mods are read once at start-up;
after adding, removing or changing a mod, restart the game. What was loaded
is printed to the console as `[mods] ...` lines.

## Layout of a mod

```
mods/
  my-mod/
    mod.ini                 # optional metadata and load priority
    romfs/<path>            # replaces (or adds) a whole RomFS file
    fa/<archive>/<entry>    # replaces one entry inside a .fa archive
    scripts/main.lua        # Lua script entry point
```

A mod may use any combination of these. `mod.ini`:

```ini
name = my-mod
version = 1
author = you
priority = 0
```

**Priority:** mods are applied in ascending `priority`, ties by folder name.
When two mods replace the same file or set the same input field, the one
applied later (higher priority) wins; conflicts are logged.

## Replacing files

The game keeps almost all of its content (scripts, text, graphics) in three
Level-5 `ARC0` archives: `lt6/arc/lt6_a.fa`, `lt6/arc/lt6_b.fa` and
`lt6/arc/lt6_uk.fa`. Music (`.bcstm`) and videos (`.moflex`) are plain RomFS
files.

* **Whole RomFS files:** put the replacement at the same path under
  `romfs/`, e.g. `mods/my-music/romfs/lt6/snd/uk/stream/BGM_002.dspadpcm.bcstm`.
  New files and folders are allowed. Path matching ignores case.
* **Single archive entries:** `fa/<RomFS path of the archive>/<entry path>`,
  e.g. `mods/my-text/fa/lt6/arc/lt6_uk.fa/txt/uk/01/01_010020.xs`.
  Only existing entries can be replaced; new entries are ignored (logged).
  Replacement entries may be stored uncompressed.

The original image is never modified; the port builds a virtual RomFS at
start-up and redirects reads.

### Modkit

`tools/modkit.py` lists and extracts content from your local RomFS image
(`out/romfs_image.bin`, created by the build from your dump):

```powershell
python tools/modkit.py romfs-list --find bcstm
python tools/modkit.py romfs-extract lt6/snd/uk/stream/BGM_002.dspadpcm.bcstm -o scratch/bgm.bcstm
python tools/modkit.py fa-list lt6/arc/lt6_uk.fa --find txt/uk/01
python tools/modkit.py fa-extract lt6/arc/lt6_uk.fa txt/uk/01/01_010020.xs -o scratch/dialog.xs --xscr
python tools/modkit.py make-testmod     # example text mod -> mods/testmod-dialog/
```

`--xscr` additionally unpacks the three sections of a script/text block
(`.xs`); section 2 holds the text. `make-testmod` replaces the first line of
the first dialog in Froenborg with `** MODKIT TEST **` and is a good template
for text mods.

**Current limits:** replacement text must have the same length as the
original (longer text needs the XSCR offset table, which is not decoded
yet); archive entries can be replaced, not added.

## Lua scripts

A mod with `scripts/main.lua` runs Lua 5.4. The main chunk runs once at
start-up; register callbacks there:

```lua
on_load(function()
  log("hello from " .. mod.name)
end)

on_frame(function(frame)
  if host.key("F6") then
    local m = host.mouse()
    if m.inside_touch then
      input.set{ touch = true, touch_x = m.x, touch_y = m.y }
    end
  end
end)
```

`on_frame` runs once per guest VBlank (60 times per second of game time)
while all game threads are paused, so reading and writing game memory there
is safe.

### API

| Function | Description |
|---|---|
| `on_load(fn)` | `fn()` once after all mods' main chunks (call only from the main chunk) |
| `on_frame(fn)` | `fn(frame)` once per guest VBlank; `frame` = game frame counter |
| `mod.name`, `mod.priority` | folder name, priority from `mod.ini` |
| `mem.read_u8/u16/u32/s8/s16/s32/f32(addr)` | read game memory (little-endian) |
| `mem.write_u8/u16/u32/f32(addr, v)` | write game memory |
| `input.get()` | `{a, b, x, y, l, r, start, select, up, down, left, right, circle_x, circle_y, touch, touch_x, touch_y}` as the game will see it this frame |
| `input.set{...}` | override any of these fields until the next VBlank (only inside `on_frame`); buttons `true/false`, `circle_*` -156..156, `touch_x` 0..319, `touch_y` 0..239 (clamped) |
| `host.mouse()` | `{x, y, inside_touch, down, over_window}`: mouse in touch-screen pixels, also without a click; works with one window and with `--split-screens` |
| `host.key(name)` | is a host key held? Names as in `port.ini` (`"F6"`, `"A"`, `"SPACE"`, `"0x41"`); read-only, the key still works normally |
| `host.set_cursor(mode)` | `"arrow"`, `"hidden"` or `"crosshair"` over the game windows |
| `guest.frame()`, `guest.vblank()`, `guest.time_ns()` | game frame, VBlank counter, game time since start in ns |
| `log(...)`, `print(...)` | write `[mod:<name>] ...` to the console/log |
| `require("name")` | load `scripts/name.lua` of the same mod (`a.b` = `scripts/a/b.lua`) |

Invalid addresses, unknown input fields or key names raise a Lua error.

### Sandbox and limits

* Available: base, `string`, `table`, `math`, `utf8`, `coroutine`.
  Not available: `io`, `os`, `debug`, `package`, `dofile`, `loadfile`,
  `string.dump`; `load` accepts text only. Mods cannot read files, start
  programs or load DLLs.
* `math.random` has a fixed seed; the only clock is `guest.time_ns()`.
  Avoid relying on `pairs` order over table keys or `tostring(table)` if
  your mod must be reproducible.
* Each call (main chunk, `on_load`, every `on_frame`) may run at most
  50 million Lua instructions; the VM may use at most 256 MiB.
* An error (syntax, runtime, budget) is logged with file, line and stack
  traceback; that mod is then disabled, the game and other mods continue.
* With `--deterministic` (test runs) `host.*` is neutral: no mouse, no keys.

### Examples

`mods-examples/` contains two Lua mods (our own code, no game content).
Copy one into `mods/` to try it:

* `frame-log` only reads and logs (memory, input, time; uses `require`).
* `input-demo`: hold **F6** to touch the touch screen at the mouse position
  without clicking; the pointer becomes a crosshair over the touch screen.
  It also taps "New Game" once at game frame 2550.

```powershell
Copy-Item -Recurse mods-examples\input-demo mods\input-demo
.\out\port.exe --mods
```

## Not available yet

* **Function hooks** (run Lua before/after/instead of a game function) –
  planned as stage C.
* **Drawing** (`draw.text`, `draw.rect` overlays).
* Longer replacement text, adding archive entries.

Planned quality-of-life example: the magnifying glass in investigation mode
follows the mouse and a click investigates.
