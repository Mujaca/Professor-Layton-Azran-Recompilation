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

## Requirements

* Windows 10/11 (x64), a GPU with Direct3D 11
* Python 3.10+
* A **decrypted** dump of the EUR cartridge (`.3ds`, e.g. made with
  GodMode9 on your own console)

## Building

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
   .\out\port.exe --renderer hw
   ```

   Without `--renderer hw` the software rasterizer is used (slower, but the
   reference implementation).

## Controls

The top screen and the touch screen are shown in one window; click the lower
screen to touch. Default keys (configurable in `out/port.ini`, created on first
start): A = `X`, B = `Z`, X = `S`, Y = `A`, L = `Q`, R = `W`, Start = `Enter`,
Select = `Backspace`, D-Pad = arrow keys, Circle Pad = `I`/`J`/`K`/`L`.
An XInput gamepad works as well. Save games are stored in `out/savedata`.

## Repository layout

```
tools/recomp/      ARM11 -> C recompiler (game-agnostic)
tools/workarounds/ game-specific deviations (debt list, currently empty)
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
