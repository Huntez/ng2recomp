# Shaders

NG2's shaders are translated ahead of time, as Unleashed Recompiled does it:
XenosRecomp turns each Xbox 360 shader container into HLSL and DXC compiles
that to SPIR-V. The result is linked into the executable as a zstd-compressed
cache (`ng2_shaders`, from `src/gpu/shader_cache.h`), and the renderer looks
shaders up by the XXH3 hash of the guest container. No DXIL: Vulkan only.

## Where the shaders are

All 663 containers (426 vertex, 237 pixel; 625 distinct) are in the XEX's
read-only data, 0x8200AC88-0x82240698. The `.ng2` archives contain none that a
byte scan can see, but they mix raw and compressed blocks; the renderer's
CreateVertexShader/CreatePixelShader hooks must report any hash missing from
the cache, so a shader that only appears at runtime is noticed.

## Build

`CMakeLists.txt` does it on every build where the inputs changed:

1. `ng2_dump_image` loads `game_files/default.xex` with ReXGlue in tool mode
   and writes the decrypted, decompressed image (identical to the one the game
   runs with).
2. XenosRecomp (built as an ExternalProject from `thirdparty/XenosRecomp`,
   upstream 990d03b plus `patches/xenosrecomp/`) scans it and writes
   `shaders/ng2_shader_cache.cpp` in the build directory. Any shader that fails
   to compile fails the build.

`NG2_GAME_DIR` points at another extraction of the game.

Debugging single shaders: `tools/extract_shaders.py <image> <dir>` writes each
container to its own file; `XenosRecomp <file> <out.hlsl> shader_common.h`
translates one.

## What NG2 needed beyond Unleashed

The patch (`patches/xenosrecomp/0001-*.patch`):

- Memory export. 64 vertex shaders are GPU passes drawn one vertex per output
  (skinning and the like): they read their inputs by index and write results
  with `eA`/`eM#` exports, which later draws use as vertex buffers. Ported
  from Xenia's SPIR-V translator (format packing, endian swap, bounds).
- Raw vertex fetches: 330 vertex shaders fetch without a declared vertex
  element; those fetches read guest memory through the vertex fetch
  constants. Full fetches latch the index for the mini fetches after them.
- The vertex index in r0.x, as the hardware provides it (Unleashed only
  needed it for instancing).
- Bool-conditional exec blocks (`cexec`) were run unconditionally upstream;
  NG2 has about 4000 of them.
- Bool constants as the hardware addresses them: 256 bits (VS b0-127, PS
  b128-255). Unleashed packed 16 per stage; NG2 uses at least b33 and
  unnamed ones.
- 32 texture fetch slots (vertex textures use s16/s17) and 1D textures,
  sampled as Nx1 2D textures. Vertex shaders sample mip 0.
- No vertex input attributes: every vertex fetch reads guest memory. Fetches
  of declared elements are placeholders in the microcode (D3D patches them
  from the vertex declaration on the console), so the renderer supplies each
  element slot's fetch constant, offset, stride and format at draw time.
- An NDC transform in every vertex shader (identity with the hardware
  viewport transform, pixels to NDC without it).
- Interpolator order TEXCOORD0-7, COLOR0-1, TEXCOORD8-15, so the rectangle
  list geometry shader can pass what pixel shaders read (TEXCOORD0-7,
  COLOR0-1) within 64 input components.

## Contract with the renderer

Push constants are three GPU addresses (vertex constants, pixel constants,
shared constants), as in Unleashed. Shared constants, in bytes:

| Offset | Contents |
| --- | --- |
| 0 | texture descriptor indices, `[2D, 3D, Cube, 1D][32 slots]` (uint) |
| 512 | sampler descriptor indices `[32]` |
| 640 | bool constants, 8 uints, bit N = hardware bool address N |
| 672 | swapped texcoords mask |
| 676 | alpha threshold |
| 680 | half pixel offset (float2) |
| 688 | guest memory base (uint64): GPU address of guest physical address 0 |
| 696 | the 96 vertex fetch constants, 2 dwords each |
| 1464 | 16 declared vertex elements (uint4): fetch constant, offset and stride in dwords, format flags |
| 1720 | NDC transform (float4) |
| 1736 | alpha test function (RB_COLORCONTROL alpha_func) |

The D3D12/DXIL side of the patch is not maintained (Vulkan only).

Consequence for the renderer: memexport targets and raw-fetched vertex data
are addressed by guest physical address, so vertex buffers live in one GPU
buffer laid out like guest physical memory (`g_GuestMemoryBase`), filled from
CPU writes on unlock, written by skinning shaders, and bound with offsets for
draws. Shaders need `bufferDeviceAddress` and `vertexPipelineStoresAndAtomics`.
