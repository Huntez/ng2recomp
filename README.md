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

Requirements: clang 20+, CMake 3.25+, Ninja, Vulkan headers/loader, GTK3.

1. Build and install the SDK (pinned at v0.10.0):

       git clone --recursive https://github.com/rexglue/rexglue-sdk thirdparty/rexglue-sdk
       cd thirdparty/rexglue-sdk && git checkout v0.10.0 && git submodule update --init --recursive
       cmake --preset linux-amd64
       cmake --build out/build/linux-amd64 --config Release --target install

2. Extract the disc (World release, title 544307D5) into `game_files/`:

       python3 tools/xdvdfs_extract.py "Ninja Gaiden II (World).iso" game_files

3. Build (runs codegen first, ~560 MB of generated C++):

       cmake --preset linux-amd64-release
       cmake --build out/build/linux-amd64-release

4. Run:

       out/build/linux-amd64-release/ng2 --gpu_plugin=xenos --game_data_root=game_files

## Layout

- `ng2_manifest.toml`: ReXGlue project manifest.
- `ng2_codegen.toml`: codegen overrides (function boundaries, fiber hooks,
  setjmp/longjmp).
- `ng2_pointer_funcs.toml`: functions only reached through pointers, found by
  `tools/find_pointer_funcs.py` on an image dumped with `NG2_DUMP_IMAGE=<file>`.
- `src/`: the host application.
- `tools/run-headless.sh`: silent test runs in a headless gamescope
  (`NG2_SHOTS="30 60"` takes screenshots, `NG2_WRAP` runs under gdb).

## Environment

- `NG2_AUDIO=0`: no audio device; the game's audio still runs.
- `NG2_DUMP_IMAGE=<file>`: write the loaded XEX image for analysis.
