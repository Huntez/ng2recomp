// ng2 - Ninja Gaiden II static recompilation
//
// Native renderer: the game's D3D calls are hooked and drawn with Vulkan
// through plume, after Unleashed Recompiled's gpu/video.cpp. Enabled with
// NG2_RENDERER=native; otherwise ReXGlue's xenos plugin emulates the GPU.

#pragma once

#include <cstdint>

namespace ng2::video {

// True when NG2_RENDERER=native: no GPU plugin, this renderer draws.
bool IsNativeRenderer();

// Creates the Vulkan device and swapchain on the game window (Wayland or X11).
bool CreateHostDevice();

void DestroyHostDevice();

// Called from the D3D hooks on the game's D3D thread, after the original
// function ran (guest addresses; docs/d3d-map-*.md has the signatures).
void OnClear(uint32_t device, uint32_t rectCount, const int32_t (*rects)[4], uint32_t flags,
             uint32_t color, float z, uint32_t stencil);
void OnResolve(uint32_t device, uint32_t flags, uint32_t sourceRect, uint32_t destTexture,
               uint32_t destPoint, uint32_t clearColor, float clearZ, uint32_t clearStencil);
void OnSwap(uint32_t device, uint32_t frontBuffer);

}  // namespace ng2::video
