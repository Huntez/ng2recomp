# Third-party components

This project's own code is under the GNU General Public License, version 3 or (at your option) any
later version (GPL-3.0-or-later, [LICENSE](LICENSE)). It is built from, and its packages contain, the
following parts, each under its own license. None of them is copied into this repository: they are
checked out into `thirdparty/` at pinned versions, with this port's changes applied from `patches/`.

## Built into the game

| Component | Use | License |
|---|---|---|
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (`librexruntime`, `librexgpu-xenos`) | the recompiler and Xbox 360 runtime, its GPU emulation | BSD-3-Clause (its `LICENSE`; parts derived from Xenia, BSD-3-Clause) |
| [plume](https://github.com/renderbag/plume) | the Vulkan layer of the native renderer | MIT |
| volk, Vulkan Memory Allocator (through plume) | Vulkan loading and memory | MIT |
| Vulkan-Headers (through plume) | | Apache-2.0 / MIT |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) | translates the game's shaders at build time; its `smol-v` is compiled in | MIT |
| [smol-v](https://github.com/aras-p/smol-v) | compressed SPIR-V | MIT |
| SDL 3 (built into the ReXGlue runtime) | window, input, audio output | zlib |
| [xxHash](https://github.com/Cyan4973/xxHash) | hashing | BSD-2-Clause |

The native renderer (`src/gpu/video.cpp`) follows the Vulkan renderer of
[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) (GPL-3.0).

## From the system

The test AppImages don't carry these; they come from the system:

| Library | License |
|---|---|
| FFmpeg (libavformat, libavcodec, libswresample, libswscale) | LGPL-2.1-or-later (or GPL, depending on how the system's build was configured) |
| zstd (the embedded shader cache) | BSD-3-Clause |
| The Vulkan loader | Apache-2.0 |
