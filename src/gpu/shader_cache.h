// ng2 - Ninja Gaiden II static recompilation
//
// Shaders translated ahead of time by XenosRecomp (build step, see
// CMakeLists.txt). Entries are sorted by the XXH3 hash of the guest shader
// container; offsets point into the decompressed SPIR-V cache (smol-v
// encoded, zstd compressed as a whole). There is no DXIL: Vulkan only.

#pragma once

#include <cstddef>
#include <cstdint>

struct ShaderCacheEntry {
  const uint64_t hash;
  const uint32_t dxilOffset;
  const uint32_t dxilSize;
  const uint32_t spirvOffset;
  const uint32_t spirvSize;
  const uint32_t specConstantsMask;
  // Vertex fetch constants 0-95 the shader reads directly (vfetchRaw);
  // declared elements' streams come from the vertex declaration per draw.
  const uint32_t vertexFetchMask[3];
  // Float constant registers (0-255) and texture fetch slots (0-31) the shader
  // reads: the others aren't uploaded or looked up for its draws.
  const uint64_t floatConstantMask[4];
  const uint32_t textureSlotMask;
  struct GuestShader* guestShader;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
// Defined by caches that carry floatConstantMask and textureSlotMask (an older
// one fails to link instead of drawing with no constants or textures).
extern const int g_shaderCacheHasMasks;
extern const size_t g_shaderCacheEntryCount;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
