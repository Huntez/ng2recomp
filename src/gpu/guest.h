// ng2 - Ninja Gaiden II static recompilation
//
// Reading guest memory from the renderer: big-endian loads from virtual and
// physical addresses.

#pragma once

#include <cstdint>

#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

namespace ng2::guest {

inline uint8_t* Virtual(uint32_t address) {
  return rex::Runtime::instance()->memory()->TranslateVirtual<uint8_t*>(address);
}

inline uint8_t* Physical(uint32_t address) {
  return rex::Runtime::instance()->memory()->TranslatePhysical<uint8_t*>(address);
}

inline uint32_t Load32(uint32_t address) {
  return rex::byte_swap(*reinterpret_cast<const uint32_t*>(Virtual(address)));
}

inline uint16_t Load16(uint32_t address) {
  return rex::byte_swap(*reinterpret_cast<const uint16_t*>(Virtual(address)));
}

inline int32_t LoadS32(uint32_t address) {
  return int32_t(Load32(address));
}

inline float LoadFloat(uint32_t address) {
  uint32_t bits = Load32(address);
  float value;
  __builtin_memcpy(&value, &bits, sizeof(value));
  return value;
}

// D3D device fields (docs/d3d-map-*.md).
namespace device {
constexpr uint32_t kRenderTargets = 0x3090;  // 4 x D3DSurface*
constexpr uint32_t kDepthStencil = 0x30A0;   // D3DSurface*
}  // namespace device

// A texture's GPUTEXTURE_FETCH_CONSTANT (6 dwords) sits at +28.
constexpr uint32_t kTextureFetchConstant = 28;

}  // namespace ng2::guest
