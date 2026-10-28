# Ninja Gaiden II recompiled

A static recompilation of Ninja Gaiden II (Xbox 360, 2008) to native Linux,
built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). The
renderer is being rebuilt after the Vulkan path of
[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp); until
then ReXGlue's Xenos GPU emulation plugin draws the game.

You need your own copy of the game. Nothing from the disc is in this repository.

## Status

Boots to the title screen on Linux (Vulkan, xenos plugin). Not playable yet.

## Build

Requirements: clang 20+, CMake 3.25+, Ninja, Vulkan headers/loader. No GTK
(ReXGlue's docs list GTK3, but v0.10 does not use it); dialogs will use Qt.

1. Build and install the SDK (pinned at v0.10.0):

       git clone --recursive https://github.com/rexglue/rexglue-sdk thirdparty/rexglue-sdk
       cd thirdparty/rexglue-sdk && git checkout v0.10.0 && git submodule update --init --recursive
       git am ../../patches/rexglue-sdk/*.patch
       cmake --preset linux-amd64
       cmake --build out/build/linux-amd64 --config Release --target install

2. Get XenosRecomp (shader translator) with the NG2 patch:

       git clone --recursive https://github.com/hedge-dev/XenosRecomp thirdparty/XenosRecomp
       cd thirdparty/XenosRecomp && git checkout 990d03b && git submodule update --init --recursive
       git am ../../patches/xenosrecomp/*.patch

   And plume (Vulkan RHI from Unleashed Recompiled) with Wayland support:

       git clone https://github.com/renderbag/plume thirdparty/plume
       cd thirdparty/plume && git checkout 1192686 && git submodule update --init contrib/volk contrib/VulkanMemoryAllocator contrib/Vulkan-Headers
       git am ../../patches/plume/*.patch

3. Extract the disc (World release, title 544307D5) into `game_files/`:

       python3 tools/xdvdfs_extract.py "Ninja Gaiden II (World).iso" game_files

4. Build (runs codegen, ~560 MB of generated C++, and translates the
   game's shaders, see docs/shaders.md):

       cmake --preset linux-amd64-release
       cmake --build out/build/linux-amd64-release

5. Run:

       out/build/linux-amd64-release/ng2 --gpu_plugin=xenos --game_data_root=game_files

## Layout

- `ng2_manifest.toml`: ReXGlue project manifest.
- `ng2_codegen.toml`: codegen overrides (function boundaries, fiber hooks,
  setjmp/longjmp).
- `ng2_pointer_funcs.toml`: functions only reached through pointers, found by
  `tools/find_pointer_funcs.py` on an image dumped with `NG2_DUMP_IMAGE=<file>`.
- `src/`: the host application.
- `patches/xenosrecomp/`: XenosRecomp changes for NG2's shaders.
- `docs/`: renderer plan, shader pipeline, D3D function table, NGB reuse notes.
- `tools/run-headless.sh`: silent test runs in a headless gamescope
  (`NG2_SHOTS="30 60"` takes screenshots, `NG2_WRAP` runs under gdb).

## Environment

- `NG2_AUDIO=0`: no audio device; the game's audio still runs.
- `NG2_RENDERER=native`: the native Vulkan renderer instead of GPU emulation
  (work in progress: menus and the first chapter render). The GPU plugin's null
  backend (patches/rexglue-sdk) keeps the game's D3D library running.
  Wayland or X11, whichever SDL uses (`--video_driver=wayland|x11` to choose).
- `NG2_DUMP_IMAGE=<file>`: write the loaded XEX image for analysis.
- `NG2_D3D_STATS=1`: log D3D draw/clear/resolve/swap counts every second.
- `NG2_PAD_SCRIPT=12000:start,16000:a,19000:a:400`: a scripted controller for
  test runs (`<ms>:<button>[+<button>...][:<hold ms>]`, `@file` reads a file);
  `NG2_PAD_LIVE=<file>` presses each line appended to the file. Buttons and
  details in src/script_input.h. Add `--headless` so system prompts (storage
  device) take their defaults.
