# Native renderer plan

Goal: draw NG2 with Vulkan from hooked D3D calls, in the spirit of Unleashed
Recompiled's `gpu/video.cpp` (plume RHI, Vulkan only), instead of emulating
the Xenos GPU. It draws by default; ReXGlue's xenos plugin (`NG2_RENDERER=xenos
--gpu_plugin=xenos`) stays as the reference picture.

## Architecture (as built)

- **The game's D3D library keeps running.** Much of NG2's D3D use is inlined
  in game code (vertex declarations, streams, constants written straight into
  the device), so hooking setters alone would miss state. Instead the library
  runs unmodified. Since 2026-11-03 there is no GPU emulation at all: no GPU
  plugin is loaded (src/gpu/native_gpu.cpp is the graphics system). Direct3D's
  command stream is reported consumed at each kick and never read; what the
  game can see of the GPU (fences, the callback handshakes, the flip and its
  vsync wait, vblank interrupts) is done at the Direct3D calls that ask for it
  (InsertFence 8375F810, the handshake 8375FD30, the flip 8376A518; TU3
  addresses). Before that the xenos plugin's null backend (SDK patch 0001)
  parsed the ring for them: about 5% of the CPU in a fight.
- **The renderer hooks the points where work happens** (src/gpu/d3d_hooks.cpp):
  DrawVertices 8373C720, DrawIndexedVertices 8373CB08, BeginVertices
  8373BD50, BeginIndexedVertices 8373C1F0, Clear 837390C8, Resolve 8373D570,
  Swap 837452C0. Each calls the original, then reads the device.
- **State comes from the device's GPU register shadows**, which the D3D
  setters keep in Xenos register format (dev+0x2880 + 4*(reg-0x2000):
  RB_COLOR_INFO, RB_DEPTHCONTROL, RB_BLENDCONTROL, PA_SU_SC_MODE_CNTL,
  viewport and scissor registers, ...), the fetch constants (dev+0x480, 32 x
  24 bytes: textures and vertex streams), the shader objects (dev+0x3190 VS,
  dev+0x318C PS), the float constants (dev+0x780, dev+0x1780) and bool
  constants (dev+0x2780). Xenos semantics are documented in ReXGlue's
  include/rex/graphics/xenos.h and registers; this keeps the translation
  independent of D3D enums.
- **Shaders** are translated ahead of time (docs/shaders.md); the renderer
  maps a shader object to its cache entry by hashing the container the game
  passed to CreateVertexShader/CreatePixelShader (hook those).
- **Presentation**: Vulkan device and swapchain on the game window through
  plume (patched for Wayland, patches/plume), Wayland or X11.

## Status (2026-10-28)

The title screen renders natively and matches the xenos reference:
render targets, Clear, Resolve (copy, MSAA resolve, format conversion by
draw), Swap/present, draws (DrawVertices, DrawIndexedVertices, inline
BeginVertices/BeginIndexedVertices), shaders from the ahead-of-time cache,
2D/1D textures (base level), rect lists (geometry shader), quad lists.
Debug aids: NG2_TRACE_FRAME=<n>[+count] (ng2.log rotates at 5 MB: read ng2.1.log first), NG2_DUMP_TEXTURES=<dir> (DXT as DDS),
NG2_DEBUG_WHITE_TEXTURES=1.

## Next

- Gameplay: test runs can now press buttons (NG2_PAD_SCRIPT/NG2_PAD_LIVE).
  Start, A x3 reaches the Chapter 1 card and the first 3D scene renders;
  next is comparing in-game frames with xenos.
- Performance (2026-10-28): the opening cutscene runs at 60 fps with
  ~650 draws and ~33 resolves a frame (NG2_PROFILE=1: about 150 ms a second
  in the renderer on the game thread). Guest memory is synced only for the
  fetch constants a shader reads (XenosRecomp's per-shader mask plus the
  declaration's streams). A range unchanged for a frame is checked once a
  frame; one that changed recently (dynamic buffers filled in batches) on
  every draw; inline data always. Not handled: overlapping ranges with
  separate hashes, CPU uploads overwriting memexport results, more than 16
  declared elements.
- The xenos plugin draws the in-game cutscene black (only the credits), so
  in-game frames have no emulated reference; judge them on their own.
- Depth resolves: copied by a draw (sample 0 when multisampled) into
  R32_FLOAT. Resolved textures are keyed by address and shape (two shadow
  map sizes share an address), with the latest resolve per address as a
  fallback for other views.
- Multiple render targets: targets 0-3 while they match target 0's shape,
  per-target format, blend and color mask; a target the pixel shader
  doesn't write keeps its contents. Clear handles targets 1-3.
- EDRAM aliasing, first step: surfaces are keyed by host format, so guest
  formats sharing one (2_10_10_10_FLOAT and its AS_16_16_16_16 view, the two
  depth formats) share a texture; the main pass draws through both. Not
  handled: overlapping surfaces of different sizes or host formats.
- Movies (src/movie, 2026-10-28): the XMedia2 player is replaced by host
  decoding (FFmpeg) behind the player's method wrappers; the original's GPU
  assisted WMV decoder stalled or cut movies short. The stand-in has to
  make the original's physical allocations at create: without them the
  title movie's texture setup clears the archive's table of contents
  (0xEBEB0000, a latent game bug the original layout hides) and the title
  never loads its font and sprites. Full-screen movies (+80, drawn by the
  player itself) are not drawn yet.
- Opening stalls (fixed 2026-10-28): (1) ~97 s, the game's job system lost
  a wake-up: ReXGlue's semaphores gave released units to whoever looked
  first and SignalAndWait wasn't atomic (patches/rexglue-sdk/0005);
  NG2_DEBUG_WAITS=1 (src/debug_waits.cpp) found it. (2) ~139 s, a GPU
  hang: a vertex fetch with a garbage index read past the 512 MB guest
  memory buffer (page fault, ring timeout); addresses now wrap at 512 MB
  (patches/xenosrecomp/0003). The opening now plays into gameplay.
- Gameplay (Chapter 1 start) renders: Ryu, enemies, level, HUD, blood.
  The "missing characters" were a depth bug: the game uses a reversed
  viewport depth range (negative zScale) and the renderer sorted min/max,
  so far geometry won over near. The range now goes to Vulkan as is.
  Still to check: distant buildings look flat and untextured.
  NG2_DEBUG_HIGHLIGHT=<vs hash> or cycle:<seconds> paints a vertex
  shader's draws magenta (finding which draws are which); =all paints each
  skinned vertex shader in its own color (legend in the log).
- Characters (10-29): faces and clothes showed their insides because the
  cull mapping assumed plume's front face was counter-clockwise (it is
  clockwise; Xenos's is counter-clockwise unless PA_SU_SC_MODE_CNTL.face).
  Hair was black where a 1x1 tint texture read as black: small textures
  with packed mips keep their base level in the mip tail, and base rows
  are aligned to 32 blocks. Draws without a pixel shader (depth only) are drawn now.
- Gameplay (10-29): textures have their mips now (Xenia's guest layout), so
  distant windows and floors no longer shimmer; shadow casters get the
  game's polygon offset (no acne). Lines at the HUD's edges were the last
  row of tiled textures cut off: a tiled level can reach past pitch x rows
  (Xenia's GetTiledAddressUpperBound2D). Not done: anisotropy (not enabled
  on the device) and per-fetch LOD overrides (XenosRecomp samples with
  implicit LOD).
- Brightness (10-29): the whole picture was brighter than Xenia (menu
  included) because the display gamma table was never applied. D3D's
  SetGammaRamp leaves it at dev+0x3A8C (3 x 256 u16, already adjusted for the
  BT.709 display the kernel reports); the present shader applies it. 7e3
  render targets resolve through a shader that clamps and rounds to 7e3 and
  2-bit alpha (no visible change here). Not done: the PWL form of the table
  (10-bit front buffers), blend constants (plume pipelines keep them at 0).
- Settings (10-29), as cvars in ng2.toml or on the command line:
  render_scale 1-4 (render targets and their resolves at that many times the
  game's size; guest pixel values scaled where they reach the host; shaders
  see guest pixels and texels via XenosRecomp patch 0007; changes apply
  between frames), vsync, anisotropic_filtering (-1 game, 0 off, 1-5 for
  1x-16x). The present shader box-filters a front buffer bigger than the
  window. bloom 0/1/2 (off, half, on) acts on the bloom texture read from
  slot 1 by tone mapping (ps 2CB124864F1A4460) and the final composite (ps
  EAE805C57979C8A5); the temporal pass still reads it all. half_pixel_offset
  moves geometry half a pixel for D3D pixel centers (default on, as Xenia).
- The xenos plugin draws the in-game 3D black (only the HUD shows), so it
  is no reference for gameplay either.
- Movie audio is pushed as it decodes, paced by the video queue (about
  200 ms ahead): a long hitch underruns it and audio then trails video.
- Resolved textures are never freed (one per address and shape); a later
  CPU texture at a once-resolved address keeps sampling the GPU copy.
- The opening still looks dim and flat; with no emulated reference in-game,
  check against captures from hardware.
- Mipmaps, cube and 3D textures, the formats that need conversion
  (5_6_5, 1_5_5_5, 4_4_4_4, 2_10_10_10, 10_11_11, 11_11_10).
- Depth resolves, polygon offset, MSAA
  alpha-to-coverage, gamma (k_8_8_8_8_GAMMA, PWL ramp).
- EDRAM aliasing between surfaces that share EDRAM (Xenia's render target
  cache is the reference).
- Performance: present on a render thread like Unleashed; cheaper guest
  memory and texture invalidation than per-frame hashing.
- Review notes for the present path (window size on the UI thread, acquire
  timeout, window handle from ReXGlue).
- From the draw path review (2026-10-28), not done yet:
  - guest memory sync: only the fetch constants the shader reads, dirty-page
    tracking instead of hashing, ranges written by memory export owned by
    the GPU (a CPU upload must not overwrite skinning results);
  - textures: eviction, cheaper invalidation than hashing every frame,
    resolved textures should honour the fetch's swizzle/format, depth
    resolves, Resolve's level/slice arguments (r8/r9);
  - blend constant color, alpha test function, RB_STENCILREFMASK_BF,
    stencil reference out of the pipeline key, quad strips and indexed quad
    lists, signed texture components, packed mip tails;
  - Present holds the renderer mutex through swapchain acquire;
  - pipelines are created on the game thread (hitches until a pipeline
    cache exists).

Audio: ReXGlue's SDL3 output with its XMA decoder (libavcodec); Unleashed's
SDL driver only if this turns out worse.

License: copying Unleashed code makes this project GPL-3.0-or-later
(Unleashed is GPL-3.0; ReXGlue is BSD-3-Clause; plume and XenosRecomp MIT).

## Stutter: pipelines (10-30)

- log_hitches (or NG2_HITCHES=1) logs frames over 25 ms (and 1.6x the one
  before: cutscenes run at 30 fps) with the time per renderer stage and what
  was made. On a first run (no driver cache) the level load spent 764 ms
  making 62 pipelines and new areas 75-600 ms; single pipelines take 17-75
  ms on RADV. Textures were small by comparison.
- Pipelines are now made on worker threads (async_pipelines, on by default):
  a draw whose pipeline isn't made yet is left out until it is (a frame or
  two). Every pipeline key made is appended to ng2-pipelines.bin next to the
  config (header "NG2P", version, key size; whole 96-byte keys, a torn last
  one trimmed), and those are asked of the workers at start, behind any a
  draw needs. With the list from one run, the next had no pipeline hitches.
- Left: game-side frames (loading, renderer ~1 ms) and the title's label
  drawing from the atlas glyphs (~48 ms once).

## Performance against GPU emulation and Xenia (10-30)

Same scene (Chapter 1's first fight, from the scripted pad), headless gamescope 1280x720, RX 9070
(RADV), 30 s windows; GPU busy from amdgpu's gpu_busy_percent (the desktop adds about 5%), CPU from
the process's /proc time. All ran at 60 fps (Xenia's not logged, held visually).

| | GPU 1x | GPU 3x | CPU cores 1x / 3x |
|---|---|---|---|
| native (NG2_RENDERER=native, render_scale) | 13% | 21% | 3.04 / 2.78 |
| recomp on the xenos plugin (draw_resolution_scale) | 80% | 85% | 3.49 / 3.43 |
| Xenia Edge (draw_resolution_scale) | 18% | 54% | 2.35 / 2.34 |

Scripts: a sampler following the test run's own process (the player's game may run too), the
recomp's pad script, and for Xenia xdotool keys inside gamescope (`--keybind_a=J`: the default A key,
semicolon, doesn't reach it there; its sign-in prompt is Right, Return). The SDK's GPU emulation is
much heavier than current Xenia Edge; the recomp's own CPU time is higher than Xenia's (its threads
spin more), worth a look.

CPU (10-30): the recomp used more than Xenia because the game's D3D wait for
the GPU (sub_8373A4D0, stepping sub_83737AE0 until the GPU's read pointer
passes a fence) spins until the next vblank, a whole core on the title screen
(95% of one thread, per perf -t). After 64 steps each step now sleeps 100 us:
title 106% -> 16% of a core, the Chapter 1 fight 3.04 -> 2.36 cores, still 60
fps.

## Audio stopping in play (10-30)

The sound sometimes stopped for good while the game ran on: two threads spun
forever in XAudio2's voice processing barrier (sub_8374F078, from
sub_8374EF40: the main pass sub_8374E740/EDE8 and a worker sub_8374E808).
Each thread sets its processor's byte (PCR r13+0x10C) in a 64-bit mask and
waits for it to hold all of them; the first to see that clears it. A thread
only left on seeing the mask whole or zero, so if the OS paused it there, the
other could clear the mask, start the next pass and set its byte in the same
mask again first, leaving both waiting (the console's hardware threads aren't
paused). The hook in d3d_hooks.cpp lets a thread also leave once its own byte
is cleared (only a completed barrier does that) and clears with a CAS from the
whole value it saw. Caught in a gdb soak 5 min in (tools: a watcher signalling
SIGUSR1 to gdb when two threads spin 30 s); two 30-min soaks clean after.
The barrier hook uses byte accesses only (the masks are 4 mod 8: an 8-byte
lock could split a cache line, which Linux may throttle): only the lowest
taking part clears, a byte at a time, its own last. Another 30-min soak clean.
Test audio (NG2_TEST_AUDIO=disk) is ~70 MB a minute: keep it off a full /tmp.

Latency (NG2_LATENCY_LOG=1, frame start to present, Chapter 1 fight): 16.63 ms
average, 18.75 worst with the GPU-wait sleep, 16.63 / 18.60 spinning; CPU 2.36
vs 3.03 cores. On Windows the sleep is a yield (a sleep there is >= 1 ms).
