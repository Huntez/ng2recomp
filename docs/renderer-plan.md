# Native renderer plan

Goal: draw NG2 with Vulkan from hooked D3D calls, in the spirit of Unleashed
Recompiled's `gpu/video.cpp` (plume RHI, Vulkan only), instead of emulating
the Xenos GPU. `NG2_RENDERER=native` selects it; ReXGlue's xenos plugin
(`--gpu_plugin=xenos`) stays as the reference picture.

## Architecture (as built)

- **The game's D3D library keeps running.** Much of NG2's D3D use is inlined
  in game code (vertex declarations, streams, constants written straight into
  the device), so hooking setters alone would miss state. Instead the library
  runs unmodified against the xenos plugin's **null backend**
  (patches/rexglue-sdk): its command processor parses the ring and retires
  fences, interrupts, vblank and swaps without rendering. Verified: 60 fps,
  same per-frame D3D work as under emulation.
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
Debug aids: NG2_TRACE_FRAME=<n>, NG2_DUMP_TEXTURES=<dir>,
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
- Hang ~100 s into the opening, on xenos too (not the renderer): the main
  thread spins on 0x84C39440 waiting for the render thread (822F16D0),
  which waits on a per-frame event while aurora12.wmv plays inside the
  cutscene; the WMV decoder's threads (838716xx) sit in self-suspend. The
  decoder is the XDK's GPU-assisted one (its own Xenos shaders for detile,
  residuals, padding: "decoder_c9e\dxva_pk" paths in the image), so it needs
  GPU results in guest memory that neither the null backend nor the xenos
  plugin give it. Options: host decoding of the game's WMV files (FFmpeg)
  behind the game's movie player API, or make the decoder's GPU work land
  in guest memory.
- Resolved textures are never freed (one per address and shape); a later
  CPU texture at a once-resolved address keeps sampling the GPU copy.
- The opening still looks dim and flat; with no emulated reference in-game,
  check against captures from hardware.
- Mipmaps, cube and 3D textures, the formats that need conversion
  (5_6_5, 1_5_5_5, 4_4_4_4, 2_10_10_10, 10_11_11, 11_11_10).
- Depth resolves, scissor (PA_SC_WINDOW_SCISSOR isn't in the device's
  register shadows; find D3D's scissor storage), polygon offset, MSAA
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
