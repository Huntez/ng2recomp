// ng2 - Ninja Gaiden II static recompilation
//
// The game's GPU without GPU emulation (native_gpu.cpp): what its Direct3D
// asks of a GPU besides drawing, done at the Direct3D calls that ask for it.

#pragma once

#include <cstdint>
#include <memory>

namespace rex::system {
class IGraphicsSystem;
}

namespace ng2::gpu {

// The graphics system the runtime gets instead of a GPU plugin.
std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeGraphicsSystem();

// From the D3D hooks (d3d_hooks.cpp), on the calling guest thread, before the
// original function runs (its arguments as they arrive).
void OnInsertFence(uint32_t device);
void OnCallbackHandshake(uint32_t device, uint32_t flags, uint32_t callback, uint32_t argument);
void OnFlipBegin(uint32_t device, uint32_t noWait);
// After the flip's own handshake: the vsync wait.
void OnFlipWait(uint32_t device, uint32_t noWait);

}  // namespace ng2::gpu
