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

## Steps

1. Map the D3D library (0x83732250-0x8374D558; XAudio follows): done in
   docs/d3d-map-a.md, -b.md, -c.md. Most textures are XGSetTextureHeader
   headers on game memory (8385CF58), not CreateTexture, so textures are
   taken from the fetch constants at draw time. -c.md also decodes the
   device's pending masks.
2. Present from Swap instead of a free-running thread; the frame = the work
   between two swaps. Review notes for that rewrite: acquire with a finite
   timeout (a hidden Wayland window can block FIFO acquire/present forever,
   hanging shutdown); take the window size from the UI thread's resize
   events instead of SDL_GetWindowSizeInPixels on the render thread; get the
   SDL window from ReXGlue's WindowSDL rather than SDL_GetWindows()[0]; no
   joinable global std::thread at exit.
3. Render targets and EDRAM: surfaces (RB_COLOR_INFO/RB_DEPTH_INFO, EDRAM
   base and pitch) become host render targets; Clear clears them; Resolve
   copies them to the destination texture's guest address, kept on the GPU
   (guest address -> host texture), as Unleashed links resolved surfaces.
4. Draws: pipeline state from registers (blend, depth/stencil, cull, fill,
   primitive type, viewport/scissor), vertex input from the vertex
   declaration + vertex fetch constants, shaders from the cache, constants
   uploaded per draw (shared-constant layout in docs/shaders.md), vertex and
   index data copied from guest memory into the guest-physical GPU buffer.
5. Textures: from texture fetch constants (address, format, size, tiling,
   endianness, mips), untiled and uploaded, cached by address with
   invalidation on CPU writes (lock/unlock hooks).
6. GPU passes with memory export (skinning): run them with the guest memory
   buffer bound so their output feeds later draws.
7. Compare against `--gpu_plugin=xenos` screenshots throughout.

Audio: ReXGlue's SDL3 output with its XMA decoder (libavcodec); Unleashed's
SDL driver only if this turns out worse.

License: copying Unleashed code makes this project GPL-3.0-or-later
(Unleashed is GPL-3.0; ReXGlue is BSD-3-Clause; plume and XenosRecomp MIT).
