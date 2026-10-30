// ng2 - Ninja Gaiden II static recompilation
//
// Native renderer: the game's D3D calls are hooked and drawn with Vulkan
// through plume, after Unleashed Recompiled's gpu/video.cpp. Enabled with
// NG2_RENDERER=native; otherwise ReXGlue's xenos plugin emulates the GPU.

#pragma once

#include <filesystem>

#include <cstdint>

namespace ng2::video {

// True when NG2_RENDERER=native: no GPU plugin, this renderer draws.
bool IsNativeRenderer();

// Creates the Vulkan device and swapchain on the game window (Wayland or X11).
bool CreateHostDevice();

void DestroyHostDevice();
// Where the pipelines made are listed, to make them early next time.
void SetPipelineListPath(const std::filesystem::path& path);

// Called from the D3D hooks on the game's D3D thread, after the original
// function ran (guest addresses; docs/d3d-map-*.md has the signatures).
void OnClear(uint32_t device, uint32_t rectCount, const int32_t (*rects)[4], uint32_t flags,
             uint32_t color, float z, uint32_t stencil);
void OnResolve(uint32_t device, uint32_t flags, uint32_t sourceRect, uint32_t destTexture,
               uint32_t destPoint, uint32_t clearColor, float clearZ, uint32_t clearStencil);
void OnSwap(uint32_t device, uint32_t frontBuffer);
void OnCreateShader(uint32_t object, uint32_t container, bool pixel);
void OnDraw(uint32_t device, uint32_t primitive, uint32_t startVertex, uint32_t count,
            bool indexed, uint32_t startIndex);
// Immediate-mode draws: the game writes the data into the returned memory
// after the call; the draw is recorded at the next hooked call.
void OnBeginVertices(uint32_t device, uint32_t primitive, uint32_t vertexCount, uint32_t stride,
                     uint32_t vertexData);
void OnBeginIndexedVertices(uint32_t device, uint32_t primitive, uint32_t baseVertex,
                            uint32_t vertexCount, uint32_t indexCount, bool wideIndices,
                            uint32_t stride, uint32_t indexData, uint32_t vertexData);

}  // namespace ng2::video
