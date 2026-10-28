// ng2 - Ninja Gaiden II static recompilation
//
// Native renderer: the game's D3D calls are hooked and drawn with Vulkan
// through plume, after Unleashed Recompiled's gpu/video.cpp. Enabled with
// NG2_RENDERER=native; otherwise ReXGlue's xenos plugin emulates the GPU.

#pragma once

namespace ng2::video {

// True when NG2_RENDERER=native: no GPU plugin, this renderer draws.
bool IsNativeRenderer();

// Creates the Vulkan device and swapchain on the game window (Wayland or X11).
bool CreateHostDevice();

void DestroyHostDevice();

}  // namespace ng2::video
