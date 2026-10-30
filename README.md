# Ninja Gaiden II Recompiled

**A PC port of Ninja Gaiden II by static recompilation.**

A native Linux version of *Ninja Gaiden II* (Xbox 360, 2008), made by statically recompiling the
game's own PowerPC code to run directly on a PC, not an emulator. It is built on the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and draws with its own Vulkan renderer, which
takes over the game's Direct3D calls instead of emulating the Xbox 360 GPU. It adds PC options in the
game's own menus: higher render resolutions, anti-aliasing, window modes, V-Sync modes and more.

**You need your own copy of the game.** This project contains none of the game's data (graphics,
sound, movies, text): on first start it asks for your disc image or extracted game folder. It is made
for the **World release** (title ID `544307D5`).

**Status: early.** The game is playable from the start of Chapter 1 on Linux, but it has not been
played through, and there are no releases yet. Test builds are posted in issues.

## Features

- Native Linux build (AppImage)
- Its own Vulkan renderer: render resolution up to 8x, anti-aliasing (MSAA up to 8x), anisotropic
  filtering, the game's bloom on, reduced or off
- Window modes (windowed, borderless, fullscreen) and window size, V-Sync (Off, FIFO, Mailbox,
  Adaptive), a frame-rate counter in the game's own font
- Render pipelines made in the background and remembered between sessions, so new areas don't
  stutter
- **RECOMP SETTINGS** on the title menu and in the in-game menu; **EXIT GAME** on the title menu
- The game's movies decoded on the PC (FFmpeg)

## Performance

Measured on one machine (Ryzen 7 9800X3D, Radeon RX 9070, Mesa RADV, Linux), in the same Chapter 1 fight,
headless at 1280x720, 30 seconds each. All held 60 fps; what differs is how hard the GPU works to do
it (GPU busy, of which about 5% is the desktop) and the CPU time used.

| | GPU busy, 1x | GPU busy, 3x | CPU cores |
|---|---|---|---|
| This port's Vulkan renderer | 13% | 21% | 2.4 |
| The same recompiled game on ReXGlue's GPU emulation (Xenia's) | 80% | 85% | 3.4-3.5 |
| Xenia Edge (the emulator) | 18% | 54% | 2.3 |

The native renderer leaves the GPU most of its time free at 3x, where GPU emulation is near its
limit; that headroom is what allows 4x-8x and MSAA on top.

## Getting started

### Linux

1. Download `NinjaGaiden2-x86_64.AppImage` from a test build and make it executable (`chmod +x`, or
   Properties → Allow executing).
2. Run it. On the first start a window asks for your copy of the game: a disc image (`.iso`) or the
   `default.xex` of an extracted copy. A disc image is extracted once into the data folder.

Needs a 64-bit Linux, a GPU with Vulkan 1.3 drivers, FFmpeg's libraries (libavformat, libavcodec,
libswresample, libswscale) and `kdialog` for the first-start window. The test AppImages use the
system's libraries, so a recent distribution is safest.

### Windows

Not yet. The code builds for Windows in principle, but there is no Windows package.

### Where things are kept

| | Linux |
|---|---|
| Extracted game (from a disc image), saves, log (`ng2.log`) | `~/.local/share/ng2-recomp` |
| Settings (`ng2.toml`), the chosen game folder, the pipeline list (`ng2-pipelines.bin`) | `~/.config/ng2-recomp` |

`NG2_GAME_DIR=<folder>` starts with that game folder instead of the remembered one.

## Settings

Everything is in the game's own menus: **RECOMP SETTINGS** on the title menu, and **Recomp
Settings** in the in-game menu (Back, under Save Method). Changes apply at once and are saved.

- **Graphics**: render scale (1x–8x, with the resolution it draws at, as "2x 2240x1168"), texture filtering (the game's, or 4x–16x anisotropic), bloom
  (off, reduced, on), anti-aliasing (off, or 2x as on the console, 4x, 8x MSAA)
- **Display**: window mode (windowed, borderless, fullscreen), window size, V-Sync (Off, FIFO,
  Mailbox, Adaptive; a mode the driver lacks falls back to FIFO), show FPS

The game's own Options (camera, controller, sound, screen) are where they always were.

Some settings have no menu entry yet; they go in `ng2.toml`:

- `async_pipelines = false`: make render pipelines as they're needed instead of in the background
  (a new effect can stutter, but never appears a frame late)
- `log_hitches = true`: log every frame that takes over 25 ms with what the renderer spent it on
- `half_pixel_offset = false`: drop the console's half-pixel shift

## Controls

A controller works as on the Xbox 360. Keyboard and mouse go through ReXGlue's mapping and aren't
tuned for this game yet.

## Known issues

- Only Chapter 1 has been tested; later chapters may show glitches the renderer doesn't handle yet.
- Some cutscene sounds may play a little late.

## Building from source

The build needs the game's code, so it starts from your own disc image. Requirements: clang 20+,
CMake 3.25+, Ninja, Python 3, the Vulkan headers and loader, FFmpeg (libavformat, libavcodec,
libswresample, libswscale).

1. The ReXGlue SDK, pinned at v0.10.0, with this port's patches:

       git clone --recursive https://github.com/rexglue/rexglue-sdk thirdparty/rexglue-sdk
       cd thirdparty/rexglue-sdk && git checkout v0.10.0 && git submodule update --init --recursive
       git am ../../patches/rexglue-sdk/*.patch
       cmake --preset linux-amd64
       cmake --build out/build/linux-amd64 --config Release --target install

2. XenosRecomp (the shader translator) and plume (the Vulkan layer of Unleashed Recompiled), with
   this port's patches:

       git clone --recursive https://github.com/hedge-dev/XenosRecomp thirdparty/XenosRecomp
       cd thirdparty/XenosRecomp && git checkout 990d03b && git submodule update --init --recursive
       git am ../../patches/xenosrecomp/*.patch

       git clone https://github.com/renderbag/plume thirdparty/plume
       cd thirdparty/plume && git checkout 1192686 && git submodule update --init contrib/volk contrib/VulkanMemoryAllocator contrib/Vulkan-Headers
       git am ../../patches/plume/*.patch

3. Extract the disc into `game_files/`:

       python3 tools/xdvdfs_extract.py "Ninja Gaiden II (World).iso" game_files

4. Build. This generates about 560 MB of C++ from the game's code and translates its shaders
   (see [docs/shaders.md](docs/shaders.md)):

       cmake --preset linux-amd64-release
       cmake --build out/build/linux-amd64-release

5. Run it, or pack an AppImage with `packaging/appimage/build.sh`:

       NG2_RENDERER=native out/build/linux-amd64-release/ng2 --game_data_root=game_files

How it works: [docs/renderer-plan.md](docs/renderer-plan.md) (the renderer),
[docs/menus.md](docs/menus.md) (the menus), [docs/shaders.md](docs/shaders.md). How to contribute:
[CONTRIBUTING.md](CONTRIBUTING.md); the layout, tools and test rules, also for coding agents:
[AGENTS.md](AGENTS.md).

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay: the recompiler and Xbox 360
  runtime this is built on, itself derived from [Xenia](https://xenia.jp) (BSD-3-Clause)
- [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) by hedge-dev: the native
  renderer follows its Vulkan renderer (GPL-3.0)
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) by hedge-dev: the shader translator (MIT)
- [plume](https://github.com/renderbag/plume) by renderbag: the Vulkan layer (MIT)

*Ninja Gaiden* is a trademark of Koei Tecmo. This project is not affiliated with or endorsed by Koei
Tecmo or Team Ninja.

## License

This project's code is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version (`GPL-3.0-or-later`; the text is in
[LICENSE](LICENSE)). It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY.
The parts it is built from keep their own licenses: see [THIRD_PARTY.md](THIRD_PARTY.md).
