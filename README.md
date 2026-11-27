# Professor Layton and the Azran Legacy – Static Recompilation

A native Windows port of *Professor Layton and the Azran Legacy* (Nintendo 3DS,
EUR release) built by **static recompilation** – no emulator at runtime.
The game's ARM11 code is translated ahead of time into C (`tools/recomp`), and
a custom runtime (`runtime/`) provides the 3DS kernel, system services, the
PICA200 GPU (software rasterizer and a Direct3D 11 renderer), DSP audio and
input. The [Azahar](https://github.com/azahar-emu/azahar) emulator served only
as a reference for reading and comparing behaviour; none of its code is linked.

**This repository contains no game code or assets.** You need your own legally
dumped copy of the game; the build turns *your* dump into the executable on
your machine.

## Disclaimer
This project was made almost entirely with AI (mainly DeepSeek V4.1-Flash and
Claude Opus 5.5). It is a personal experiment to explore what AI is capable of.
If you dislike the use of AI, please don't use this project.

Yes, I am a software engineer, and no, I have almost no experience in this
field. Part of me simply wanted to try it. I do think AI is a powerful and
interesting tool, and I find the technology fascinating. At the same time, what
it is capable of scares me: many questions remain unanswered, and there are
things that worry me. But exploring those questions wasn't the goal of this
project.

I am well aware of both the benefits and the downsides of AI. If you just want
to harass me for using it, I will block you.

## Status

The game boots, plays its intro films, shows the title screen and menus, lets
you start a new game or continue a save, and the first puzzle is playable with
sound and saving. The 3D scenes run at about 30 guest frames per second with
the hardware renderer. This is a work-in-progress research project: expect
bugs, slow scenes and occasional hangs. Progress and milestones are tracked in
[`docs/ROADMAP.md`](docs/ROADMAP.md).

## Using the builder

The builder package (`LaytonBuilder-<version>.zip`) is the easiest way to
build the port: it bundles everything needed (an embedded Python, the Zig C
compiler, the recompiler and runtime sources and their license texts) and
installs or downloads nothing. It contains no game content.

1. You need Windows 10/11 (x64) with a Direct3D 11 GPU, about 3 GB of free
   disk space and a **decrypted** dump of the European cartridge
   (`CTR-P-AL6P`, `.3ds`, e.g. made with GodMode9 on your own console).
2. Extract the ZIP to a folder you can write to and start
   `LaytonBuilder.exe`.
3. Click **Choose dump...** (or drop the `.3ds` onto the window or onto
   `LaytonBuilder.exe`). The builder checks the dump first - it must be a
   full cartridge dump of the European release, decrypted and unmodified -
   and explains what is wrong otherwise (wrong region, encrypted dump, CIA
   file, other game, incomplete or modified dump).
4. The build runs with a progress display; the first build takes several
   minutes. The result goes to the `game` folder next to the builder.
5. Click **Play**, or use the shortcut `game\Play Professor Layton`
   (or start `game\out\port.exe`).

Your dump is only read, never copied or changed. Save games
(`game\out\savedata`), `game\out\port.ini` and the logs
(`game\out\logs`) are kept when you build again. Command line:
`LaytonBuilder.exe <dump.3ds> [--out <folder>] [--headless]`; the build log
is written to `<folder>uilder.log`.

Maintainers build the package with `python tools/make_builder.py` from the
repository and a local cache of the bundled downloads (exact files, versions
and checksums: `python tools/make_builder.py --print-artefacts`,
[`docs/TOOLS.md`](docs/TOOLS.md)).

## Requirements (building from source)

* Windows 10/11 (x64), a GPU with Direct3D 11
* Python 3.10+
* A **decrypted** dump of the EUR cartridge (`.3ds`, e.g. made with
  GodMode9 on your own console)

## Building from source

1. Place your dump in the repository root under exactly this name:
   `Professor Layton and the Azran Legacy (Europe).3ds`
2. Install the Python dependencies (the C compiler comes with `ziglang`):

   ```powershell
   pip install pyctr capstone keystone-engine ziglang pillow
   ```

3. Extract the game data and build:

   ```powershell
   python tools/extract.py          # ExeFS + RomFS inventory
   python tools/extract_romfs.py    # RomFS files -> out/romfs
   python tools/romfs_image.py      # RomFS image -> out/romfs_image.bin
   python tools/build.py            # recompile + compile -> out/port.exe (first run takes a few minutes)
   ```

4. Run:

   ```powershell
   .\out\port.exe
   ```

   (Double-clicking `out\port.exe` works too: it finds its data in the
   parent folder.)

   The Direct3D 11 renderer is the default. `--scale 2` (or `3`, `4`)
   renders at a higher internal resolution; with it, `--texture-filter
   xbrz` (or `linear`) smooths text and 2D graphics that would otherwise
   show up as blocky pixels (display only, default `none`; also
   `[display] texture_filter` in `out/port.ini`). `--split-screens` opens
   one window per screen, F11 toggles fullscreen. `--renderer soft` selects
   the software rasterizer (slower, but the reference implementation).

## Controls

The top screen and the touch screen are shown in one window; click the lower
screen to touch. Default keys (configurable in `out/port.ini`, created on first
start): A = `X`, B = `Z`, X = `S`, Y = `A`, L = `Q`, R = `W`, Start = `Enter`,
Select = `Backspace`, D-Pad = arrow keys, Circle Pad = `I`/`J`/`K`/`L`.
An XInput gamepad works as well. Save games are stored in `out/savedata`.

`port.exe` starts without a console window. Its output is always written to
`out/logs/port-<date>-<time>-<pid>.log` (the last 10 logs are kept) – attach
the newest log when reporting a bug. `--console` (or `[debug] console = 1` in
`out/port.ini`) shows the output live; `--fps` shows the game's frame rate.

## Mods

The port can load mods: replacement files (text, graphics, music, videos)
and sandboxed Lua scripts (memory access, input, mouse). Mods are off by
default; start with `--mods`. See the
[Modding Guide](docs/MODDING_GUIDE.md).

## Repository layout

```
tools/recomp/      ARM11 -> C recompiler (game-agnostic)
tools/workarounds/ game-specific deviations (debt list, currently empty)
tools/starter/     LaytonBuilder.exe (builder package starter, Win32 C)
tools/g3ds/        analysis tools (trace diff, guest refs, ...)
runtime/cpu/       CPU state, instruction semantics
runtime/kernel/    SVCs, threads, IPC, 3DS services
runtime/gpu/       PICA200 (software rasterizer, D3D11 renderer), texture decoders
runtime/audio/     DSP emulation (HLE), audio output
runtime/platform/  Win32 window, input
tests/             recompiler, GPU, audio and runtime self-tests
docs/              roadmap, reference notes, tool docs (mostly German)
```

`python tools/check.py` runs all self-tests, the build and the frontier
check (boot progress compared against a reference trace).

Most project documentation in `docs/` is written in German.
[`AGENTS.md`](AGENTS.md) describes the rules for the (AI-assisted) development
process.

## Legal

This project is not affiliated with or endorsed by Nintendo or Level-5.
*Professor Layton* is a trademark of Level-5 Inc. No ROM, game code, assets or
keys are distributed here; you must own the game and dump it yourself.

The source code is licensed under the
[GNU General Public License v3.0](LICENSE).
