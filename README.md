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

The Steam Deck is the baseline for optimization: changes are judged by whether it holds 60 fps in the
game's heaviest scenes.

Measured on a Steam Deck LCD in Game Mode, on battery, with sound, in Chapter 1's first fight
(30 seconds each, the Deck's own performance overlay), the port at the Deck's default settings (1x)
and Xenia Edge at its defaults (November 2026 builds):

| | This port | Xenia Edge |
|---|---|---|
| Average fps | 60.0 | 58.2 |
| 1% low | 55.4 | 27.5 |
| 0.1% low | 52.6 | 18.1 |
| CPU load | 32% | 51% |
| GPU load | 48% | 81% |
| Whole Deck's power draw | 11 W | 22 W |

Both average close to 60 fps, but the emulator's slowest frames drop to 18-28 fps, while the port's
stay above 52. The port draws half the power, so a charge lasts about twice as long.

## Getting started

### Linux

1. Download `NinjaGaiden2-x86_64.AppImage` from a test build and make it executable (`chmod +x`, or
   Properties → Allow executing).
2. Run it. On the first start it installs the game:
   1. Your copy of the game: a disc image (`.iso`), the Games on Demand package (the file in
      `544307D5/00007000`, next to its `.data` folder), or the `default.xex` of an extracted copy.
      A disc image or package is copied once into the data folder.
   2. Title Update 3 for the World release (`TU_1A461UL_000000G000000.00000000000O4`, or its
      `default.xexp`), which the port is built for. It goes in the game folder.
   3. Downloadable content (Mission Mode, the costume packs), if you have it: the folder with the
      packages (the console's `544307D5/00000002`). More can go in the game folder's `dlc` folder
      later; they are installed on the next start.

Needs a 64-bit Linux with glibc 2.35 or newer (SteamOS, Ubuntu 22.04+, Debian 12, Fedora 36+), a
GPU with Vulkan 1.3 drivers, Python 3 and `kdialog` for the first-start windows.

### Windows

1. Download `NinjaGaiden2-windows-x64.zip` from a test build and unpack it anywhere.
2. Run `ng2.exe`. The first time it installs the game as on Linux: your copy of the game (disc
   image, Games on Demand package or `default.xex`; a disc or package is copied into
   `%LOCALAPPDATA%\NinjaGaiden2\game`), Title Update 3, then the DLC if you have it.

Needs a GPU with Vulkan 1.3 drivers. The Microsoft Visual C++ runtime DLLs are included.

### Where things are kept

| | Linux | Windows |
|---|---|---|
| Extracted game (from a disc image), saves, log (`ng2.log`) | `~/.local/share/ng2-recomp` | `%LOCALAPPDATA%\NinjaGaiden2` |
| Settings (`ng2.toml`), the chosen game folder, the pipeline list (`ng2-pipelines.bin`) | `~/.config/ng2-recomp` | `%LOCALAPPDATA%\NinjaGaiden2` |

`NG2_GAME_DIR=<folder>` starts with that game folder instead of the remembered one (Linux; on
Windows pass `--game_data_root=<folder>`).

## Settings

Everything is in the game's own menus: **RECOMP SETTINGS** on the title menu, and **Recomp
Settings** in the in-game menu (Back, under Save Method). Changes apply at once and are saved.

- **Graphics**: render scale (1x–8x, with the resolution it draws at, as "2x 2240x1168"), texture filtering (the game's, or 4x–16x anisotropic), bloom
  (off, reduced, on), anti-aliasing (off, or 2x as on the console, 4x, 8x MSAA)
- **Display**: window mode (windowed, borderless, fullscreen), window size, aspect ratio (16:9,
  16:10, 21:9, 32:9, 4:3), V-Sync (Off, FIFO, Mailbox, Adaptive; a mode the driver lacks falls back
  to FIFO), show FPS, mouse sensitivity, button prompts (Auto follows the device in use; Xbox,
  PlayStation, Nintendo, Keyboard)
- **Cheats**: infinite health, infinite ninpo, unlock difficulties, unlock costumes, all weapons and
  ninpo (Level 1 or 3), all crystal skulls, max HP and ninpo
- **Gameplay**: skip intro, skip cutscenes (seen or not), pause in background
- **Extras**: skip death screen, auto chapter screen, HUD (shown, fade out when not fighting,
  hidden), no post-effects (no blur, no white flashes), texture LOD bias (the game's, 0, -1), smooth
  HUD and menus, hi-res font (from the next start)

In the in-game menu, a page with seven settings has no Restore Defaults row (the list has no room
for it); the title menu's has.

The game's own Options (camera, controller, sound, screen) are where they always were.

Some settings have no menu entry yet; they go in `ng2.toml`:

- `async_pipelines = false`: make render pipelines as they're needed instead of in the background
  (a new effect can stutter, but never appears a frame late)
- `log_hitches = true`: log every frame that takes over 25 ms with what the renderer spent it on
- `half_pixel_offset = false`: drop the console's half-pixel shift

## Controls

A controller works as on the Xbox 360. Keyboard and mouse work too:

| Key | Action | Key | Action |
|---|---|---|---|
| W A S D | move | mouse | camera |
| left mouse, J | quick attack (X) | right mouse, K | strong attack (Y) |
| Space | jump (A) | E | projectile (B) |
| F | ninpo (Y + B) | C | water run (A + X) |
| Shift | block (LT) | Q, middle mouse | RT |
| R | LB | V | RB |
| Z / X | stick clicks | Tab / Escape | Back / Start |
| arrows | d-pad | Return / Backspace | A / B in menus |

The keys can be changed in `ng2.toml` (`keybind_a = "Space,C,Return"` and so on). The mouse only
moves the camera while the keyboard is in use, and the pointer is hidden while a controller is.

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

3. Extract the disc (or the Games on Demand package) into `game_files/` and put Title Update 3's
   `default.xexp` next to `default.xex` (see [docs/title-update.md](docs/title-update.md)):

       python3 tools/xdvdfs_extract.py "Ninja Gaiden II (World).iso" game_files
       python3 tools/stfs_extract.py TU_1A461UL_000000G000000.00000000000O4 default.xexp game_files/default.xexp

4. Build. This generates about 560 MB of C++ from the game's code and translates its shaders
   (see [docs/shaders.md](docs/shaders.md)):

       cmake --preset linux-amd64-release
       cmake --build out/build/linux-amd64-release

5. Run it:

       out/build/linux-amd64-release/ng2 --game_data_root=game_files

The AppImage, `packaging/appimage/build.sh`, builds the SDK and the game in an Ubuntu 22.04
container (Docker; `packaging/appimage/Dockerfile`), so it needs only glibc 2.35 and runs on
SteamOS and other current distributions. It bundles a small FFmpeg with the movies' codecs and
writes `out/appimage/NinjaGaiden2-x86_64.AppImage`. `packaging/appimage/build-local.sh` packs
the native build instead, quicker but only for the machine that built it.

The Windows build is cross-compiled from Linux after the native one: `packaging/windows/build.sh`
(clang and lld against Microsoft's C runtime and Windows SDK from
[xwin](https://github.com/Jake-Shadle/xwin), `xwin --accept-license splat --output ~/.xwin`; plume
also needs its `contrib/D3D12MemoryAllocator` submodule there). It builds the SDK for Windows, fetches
FFmpeg's Windows build, and packs `out/windows/NinjaGaiden2-windows-x64.zip`.

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
- [Xelu's controller and keyboard prompts](https://thoseawesomeguys.com/prompts/) by Nicolae "Xelu"
  Berbece: the PlayStation, Nintendo and keyboard button prompts (CC0)
- [Ninja Gaiden II Master Edition](https://github.com/alicenotmech/ng2-master-edition) by
  alicenotmech: a reference for this port's own versions of some of its options

*Ninja Gaiden* is a trademark of Koei Tecmo. This project is not affiliated with or endorsed by Koei
Tecmo or Team Ninja.

## License

This project's code is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version (`GPL-3.0-or-later`; the text is in
[LICENSE](LICENSE)). It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY.
The parts it is built from keep their own licenses: see [THIRD_PARTY.md](THIRD_PARTY.md).
