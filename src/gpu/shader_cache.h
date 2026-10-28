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
  struct GuestShader* guestShader;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
