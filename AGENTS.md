# Working on this project

Notes for contributors and coding agents: how the project is laid out, how to build and test it, and
the rules that keep a test run from disturbing the machine it runs on.

## Layout

- `src/`: the hand-written part: the app and its hooks (`main.cpp`, `ng2_app.h`), the native
  renderer (`src/gpu/video.cpp`, `d3d_hooks.cpp`, shaders in `src/gpu/shaders/`), the menus
  (`recomp_settings.cpp`: RECOMP SETTINGS on the title menu and in the in-game menu, the title
  menu's entries; `gpu/menu_label.cpp`: the pictures of their labels and the Options header), the
  frame-rate counter (`perf_overlay.cpp`), movies on the host (`src/movie/`), the scripted
  controller for tests (`script_input.h`).
- `generated/default/`: the recompiled game code, generated at build time from the player's
  `default.xex`. **Never commit it**, nor the game's data (`game_files/`, `*.iso`, `*.xex`, saves,
  captures of the game's pictures).
- `ng2_manifest.toml`, `ng2_codegen.toml`, `ng2_pointer_funcs.toml`: the ReXGlue project: manifest,
  codegen overrides (function boundaries, fibers, setjmp/longjmp), and functions only reached through
  pointers (`tools/find_pointer_funcs.py` on an image dumped with `NG2_DUMP_IMAGE`).
- `patches/`: changes to the ReXGlue SDK, XenosRecomp and plume (`git am` onto the pinned
  versions in `thirdparty/`, which aren't committed).
- `packaging/`: the AppImage (`appimage/build.sh`) and the Windows icon and resources.
- `docs/`: `renderer-plan.md` (the renderer, with what was measured), `menus.md` (the game's menus,
  reverse-engineered), `shaders.md`, the D3D function maps (`d3d-map-*.md`), `xenia-notes.md`.
  The game is Title Update 2 since 2026-11-02; notes from before then use 1.0 addresses, which
  `title-update.md` maps (`tools/map_addresses.py` made the map).

## Building

See the README. After the first build, `cmake --build out/build/linux-amd64-release --target ng2`
rebuilds the app alone.

## Test runs

A test run must not disturb the machine it runs on: no window on the desktop, no sound, no input
devices, and none of the player's settings or saves.

- `tools/run-headless-windows.sh` does the same for the Windows build under Proton (umu-run).
- `tools/run-headless.sh <seconds> <out dir> [game options]` runs the game in a headless gamescope
  with its own HOME, settings (`<out dir>/ng2.toml`), user data and log (`<out dir>/ng2.log`, which
  rotates at 5 MB: read `ng2.1.log` first). `NG2_SHOTS="30 60"` takes screenshots at those seconds.
- `NG2_PAD_SCRIPT=30000:start,34000:a,...` plays a controller (`<ms>:<button>[:<hold ms>]`;
  buttons and sticks in `src/script_input.h`). From boot, `30000:start,34000:a,37000:a,40000:a,
  58000:a,63000:a,262000:a` reaches Chapter 1's first fight at about 262 s; Back about 268 s opens
  the in-game menu.
- `NG2_ROOM_JUMP=<from>:<to>` swaps a room in the story script (src/room_jump.cpp lists the rooms
  with cutscenes): with the New Game path above, `3:5` plays the cutscene before Chapter 2 at about
  80-110 s.
- The native renderer draws by default; to compare with Xenia's set `NG2_RENDERER=xenos` and add
  `--gpu_plugin=xenos --headless` (Xenia's draws cutscenes black in this build).

## Environment

- `NG2_RENDERER=xenos`: the SDK's Xenia GPU emulation draws instead of the native renderer.
- `NG2_AUDIO=0`: no audio device; the game's audio still runs.
- `NG2_CONFIG=<file>`: settings file instead of `<exe dir>/ng2.toml`.
- `NG2_PROFILE=1`: time per renderer stage, once a second. `NG2_HITCHES=1` (or `log_hitches`): slow
  frames with what they were spent on. `NG2_FPS_LOG=1`: the game's frame rate once a second.
- `NG2_TRACE_FRAME=t<sec>+<count>`: log every draw of those frames.
- F7, or `NG2_TRACE_FILE=<file>` (starts when the file appears): every draw of the next
  `NG2_TRACE_FRAMES` (60) frames and a small picture every 10, in `~/ng2-trace-<n>` (`HOME` decides
  where).
- `NG2_DUMP_TEXTURES=<dir>`: write decoded textures (PGM/PAM/DDS).
- `NG2_DEBUG_HIGHLIGHT=<vs hash>|<hash>,<hash>...|cycle:<s>|all|rigid`: draw a vertex shader's
  draws in magenta (a list or all: a colour per shader, logged; rigid: the unskinned ones;
  `NG2_DEBUG_EXCLUDE=<hash>,...` leaves some out).
- `tools/xtr_draws.py <trace.xtr>`: draws, constants (`--consts`), registers (`--regs`) and
  buffers (`--dump`) of a Xenia Edge GPU trace, to compare a frame with ours.
- `NG2_DUMP_IMAGE=<file>`: write the loaded XEX image for analysis.
- `NG2_D3D_STATS=1`: D3D draw, clear, resolve and swap counts every second.
- `NG2_DRAW_STACKS=1`: each distinct chain of guest callers behind a draw, logged once (what draws
  a picture). `NG2_BUCKET_LOG=1`: each 2D render bucket's box per second and its draw methods
  (src/aspect.cpp).
- Mission Mode is a quick way to reach bosses headless (Mission 04, Ruler of Lightning). Quick
  menu: d-pad up, then up, left and A equips the bow.
- `NG2_MOVIES=xmedia`: the game's own movie player instead of FFmpeg; `NG2_MOVIE_TRACE=1` logs it.
- `NG2_PAD_LIVE=<file>`: presses each line appended to the file.

## Reverse engineering

Ghidra (headless) on the loaded image works well for this game: import the image raw at base
`0x82000000` as `PowerPC:BE:64:A2ALT-32addr`. The recompiled C++ in `generated/default/` carries each
instruction as a comment, which helps where Ghidra stops. What is known about the menus is in
`docs/menus.md`.
