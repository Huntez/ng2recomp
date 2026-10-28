// ng2 - Ninja Gaiden II static recompilation
//
// Native renderer: Vulkan through plume (Unleashed Recompiled's RHI), driven
// by hooks on the game's D3D calls (d3d_hooks.cpp). The game's D3D library
// keeps running on the GPU plugin's null backend; at each hooked call this
// file reads the device and does the host-side work.
//
// Commands are recorded on the game's D3D thread as the calls happen and
// submitted at Swap. Render targets are host textures keyed by their EDRAM
// placement; resolves copy them into host textures keyed by the destination
// guest address, which is also how Swap finds the front buffer.

#include "video.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include <plume_render_interface.h>

#include "guest.h"

extern const uint8_t g_fullscreen_vs[];
extern const size_t g_fullscreen_vs_size;
extern const uint8_t g_present_ps[];
extern const size_t g_present_ps_size;

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

using namespace plume;
namespace xenos = rex::graphics::xenos;

namespace ng2::video {

namespace {

constexpr size_t NUM_FRAMES = 2;
constexpr RenderFormat BACKBUFFER_FORMAT = RenderFormat::B8G8R8A8_UNORM;
// Bindless heaps, laid out as the translated shaders expect (docs/shaders.md).
constexpr uint32_t TEXTURE_DESCRIPTOR_COUNT = 65536;
constexpr uint32_t SAMPLER_DESCRIPTOR_COUNT = 1024;
constexpr uint32_t PUSH_CONSTANT_SIZE = 24;  // three buffer addresses

// Descriptor slots 0-2 hold the blank 2D, 3D and cube textures.
enum : uint32_t {
  DESCRIPTOR_NULL_2D,
  DESCRIPTOR_NULL_3D,
  DESCRIPTOR_NULL_CUBE,
  DESCRIPTOR_FIRST_FREE,
};
enum : uint32_t { SAMPLER_LINEAR_CLAMP, SAMPLER_FIRST_FREE };

// A host texture with the layout it was last transitioned to.
struct HostTexture {
  std::unique_ptr<RenderTexture> texture;
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  RenderFormat format = RenderFormat::UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
  RenderSampleCounts samples = RenderSampleCount::COUNT_1;
  std::unique_ptr<RenderTextureView> view;  // for the texture heap
  uint32_t descriptor = 0;                    // in the texture heap, 0 = none
};

struct DescriptorAllocator {
  uint32_t next = DESCRIPTOR_FIRST_FREE;
  std::vector<uint32_t> freed;
  uint32_t Allocate() {
    if (!freed.empty()) {
      uint32_t index = freed.back();
      freed.pop_back();
      return index;
    }
    return next < TEXTURE_DESCRIPTOR_COUNT ? next++ : 0;
  }
  void Free(uint32_t index) {
    if (index >= DESCRIPTOR_FIRST_FREE) {
      freed.push_back(index);
    }
  }
};

std::recursive_mutex g_mutex;

std::unique_ptr<RenderInterface> g_interface;
std::unique_ptr<RenderDevice> g_device;
std::unique_ptr<RenderCommandQueue> g_queue;
std::unique_ptr<RenderCommandList> g_commandLists[NUM_FRAMES];
std::unique_ptr<RenderCommandFence> g_commandFences[NUM_FRAMES];
std::unique_ptr<RenderCommandSemaphore> g_acquireSemaphores[NUM_FRAMES];
// One per swapchain image: a present can still hold its semaphore when the
// next frame in flight starts.
std::vector<std::unique_ptr<RenderCommandSemaphore>> g_renderSemaphores;
bool g_commandListPending[NUM_FRAMES];
std::unique_ptr<RenderSwapChain> g_swapChain;
std::vector<std::unique_ptr<RenderFramebuffer>> g_swapChainFramebuffers;
bool g_swapChainValid = false;
uint32_t g_frame = 0;
bool g_recording = false;

std::unique_ptr<RenderDescriptorSet> g_textureDescriptorSet;
std::unique_ptr<RenderDescriptorSet> g_samplerDescriptorSet;
std::unique_ptr<RenderPipelineLayout> g_pipelineLayout;
std::unique_ptr<RenderSampler> g_linearClampSampler;
HostTexture g_nullTextures[DESCRIPTOR_FIRST_FREE];
std::unique_ptr<RenderTextureView> g_nullTextureViews[DESCRIPTOR_FIRST_FREE];
DescriptorAllocator g_descriptors;

std::unique_ptr<RenderShader> g_fullscreenShader;
std::unique_ptr<RenderShader> g_presentShader;
std::unique_ptr<RenderPipeline> g_presentPipeline;

// Render targets by EDRAM placement and shape.
struct SurfaceKey {
  uint32_t edramBase;
  uint32_t format;  // ColorRenderTargetFormat, or 0x100 | DepthRenderTargetFormat
  uint32_t width;
  uint32_t height;
  uint32_t msaa;
  bool operator==(const SurfaceKey&) const = default;
};
struct SurfaceKeyHash {
  size_t operator()(const SurfaceKey& k) const {
    return (size_t(k.edramBase) * 0x9E3779B1u) ^ (size_t(k.format) << 20) ^
           (size_t(k.width) << 40) ^ (size_t(k.height) << 8) ^ k.msaa;
  }
};
std::unordered_map<SurfaceKey, std::unique_ptr<HostTexture>, SurfaceKeyHash> g_surfaces;

// Textures written by resolves, by guest physical base address.
std::unordered_map<uint32_t, std::unique_ptr<HostTexture>> g_resolvedTextures;

// Framebuffers by attachment.
struct FramebufferKey {
  const HostTexture* color;
  const HostTexture* depth;
  bool operator==(const FramebufferKey&) const = default;
};
struct FramebufferKeyHash {
  size_t operator()(const FramebufferKey& k) const {
    return std::hash<const void*>()(k.color) * 31 ^ std::hash<const void*>()(k.depth);
  }
};
std::unordered_map<FramebufferKey, std::unique_ptr<RenderFramebuffer>, FramebufferKeyHash>
    g_framebuffers;

// Formats ---------------------------------------------------------------------

RenderFormat ColorTargetFormat(uint32_t format) {
  switch (xenos::ColorRenderTargetFormat(format)) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return RenderFormat::R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return RenderFormat::R16G16B16A16_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return RenderFormat::R16G16B16A16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16:
      return RenderFormat::R16G16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return RenderFormat::R16G16B16A16_SNORM;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return RenderFormat::R16G16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return RenderFormat::R32_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return RenderFormat::R32G32_FLOAT;
  }
  return RenderFormat::R8G8B8A8_UNORM;
}

// Host format for a resolve destination (a guest texture written by the GPU).
RenderFormat ResolveTextureFormat(xenos::TextureFormat format) {
  switch (format) {
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_A:
    case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
      return RenderFormat::R8G8B8A8_UNORM;
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_16_16_16_16:
      return RenderFormat::R16G16B16A16_UNORM;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
      return RenderFormat::R16G16B16A16_FLOAT;
    case xenos::TextureFormat::k_16_16:
      return RenderFormat::R16G16_SNORM;
    case xenos::TextureFormat::k_16_16_FLOAT:
      return RenderFormat::R16G16_FLOAT;
    case xenos::TextureFormat::k_32_FLOAT:
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
      return RenderFormat::R32_FLOAT;
    case xenos::TextureFormat::k_32_32_FLOAT:
      return RenderFormat::R32G32_FLOAT;
    default:
      return RenderFormat::UNKNOWN;
  }
}

RenderSampleCounts SampleCount(uint32_t msaa) {
  switch (msaa) {
    case 1:
      return RenderSampleCount::COUNT_2;
    case 2:
      return RenderSampleCount::COUNT_4;
    default:
      return RenderSampleCount::COUNT_1;
  }
}

// Frame -----------------------------------------------------------------------

void WaitForFrame(uint32_t frame) {
  if (g_commandListPending[frame]) {
    g_queue->waitForCommandFence(g_commandFences[frame].get());
    g_commandListPending[frame] = false;
  }
}

void WaitForGPU() {
  for (uint32_t i = 0; i < NUM_FRAMES; i++) {
    WaitForFrame(i);
  }
}

RenderCommandList* CommandList() {
  if (!g_recording) {
    WaitForFrame(g_frame);
    g_commandLists[g_frame]->begin();
    g_recording = true;
  }
  return g_commandLists[g_frame].get();
}

void Transition(HostTexture& texture, RenderTextureLayout layout) {
  if (texture.layout != layout) {
    CommandList()->barriers(RenderBarrierStage::GRAPHICS | RenderBarrierStage::COPY,
                            RenderTextureBarrier(texture.texture.get(), layout));
    texture.layout = layout;
  }
}

void SetDescriptor(HostTexture& texture) {
  if (texture.descriptor == 0) {
    texture.descriptor = g_descriptors.Allocate();
  }
  if (texture.descriptor != 0) {
    if (!texture.view) {
      RenderTextureViewDesc viewDesc;
      viewDesc.format = texture.format;
      viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
      viewDesc.mipLevels = 1;
      texture.view = texture.texture->createTextureView(viewDesc);
    }
    g_textureDescriptorSet->setTexture(texture.descriptor, texture.texture.get(),
                                       RenderTextureLayout::SHADER_READ, texture.view.get());
  }
}

// Render targets --------------------------------------------------------------

// The host texture for a guest D3DSurface (render target or depth/stencil),
// created on first use.
HostTexture* GetSurface(uint32_t surface, bool depth) {
  if (!surface) {
    return nullptr;
  }
  // Texture-level surfaces (Common bit 30) aren't EDRAM targets.
  if (guest::Load32(surface) & 0x40000000) {
    return nullptr;
  }
  uint32_t info = guest::Load32(surface + 28);
  uint32_t dims = guest::Load32(surface + 36);
  SurfaceKey key;
  key.edramBase = info & 0xFFF;
  key.format = depth ? (0x100 | ((info >> 16) & 1)) : ((info >> 16) & 0xF);
  key.width = ((dims >> 18) & 0x3FFF) + 1;
  key.height = ((dims >> 3) & 0x7FFF) + 1;
  key.msaa = guest::Load16(surface + 24) & 3;

  auto& slot = g_surfaces[key];
  if (!slot) {
    auto host = std::make_unique<HostTexture>();
    host->width = key.width;
    host->height = key.height;
    host->samples = SampleCount(key.msaa);
    host->format = depth ? RenderFormat::D32_FLOAT_S8_UINT : ColorTargetFormat(key.format);
    RenderTextureDesc desc = RenderTextureDesc::Texture2D(
        key.width, key.height, 1, host->format,
        depth ? RenderTextureFlag::DEPTH_TARGET : RenderTextureFlag::RENDER_TARGET);
    desc.multisampling.sampleCount = host->samples;
    host->texture = g_device->createTexture(desc);
    REXLOG_INFO("video: {} target {}x{} edram {:#x} format {} msaa {}", depth ? "depth" : "color",
                key.width, key.height, key.edramBase, key.format & 0xFF, key.msaa);
    slot = std::move(host);
  }
  return slot.get();
}

void BindFramebuffer(HostTexture* color, HostTexture* depth) {
  auto& framebuffer = g_framebuffers[{color, depth}];
  if (!framebuffer) {
    const RenderTexture* colors[] = {color ? color->texture.get() : nullptr};
    RenderFramebufferDesc desc;
    desc.colorAttachments = color ? colors : nullptr;
    desc.colorAttachmentsCount = color ? 1 : 0;
    desc.depthAttachment = depth ? depth->texture.get() : nullptr;
    framebuffer = g_device->createFramebuffer(desc);
  }
  if (color) {
    Transition(*color, RenderTextureLayout::COLOR_WRITE);
  }
  if (depth) {
    Transition(*depth, RenderTextureLayout::DEPTH_WRITE);
  }
  CommandList()->setFramebuffer(framebuffer.get());
}

// Host texture backing a guest texture written by resolves.
HostTexture* GetResolveDestination(uint32_t address, const xenos::xe_gpu_texture_fetch_t& fetch) {
  RenderFormat format = ResolveTextureFormat(fetch.format);
  if (format == RenderFormat::UNKNOWN) {
    return nullptr;
  }
  uint32_t width = fetch.size_2d.width + 1;
  uint32_t height = fetch.size_2d.height + 1;
  auto& slot = g_resolvedTextures[address];
  if (slot && (slot->width != width || slot->height != height || slot->format != format)) {
    // Same address, new shape: the old texture may still be in flight.
    if (g_recording) {
      auto* commandList = g_commandLists[g_frame].get();
      commandList->end();
      g_queue->executeCommandLists(commandList, g_commandFences[g_frame].get());
      g_commandListPending[g_frame] = true;
      g_recording = false;
    }
    WaitForGPU();
    g_descriptors.Free(slot->descriptor);
    slot.reset();
  }
  if (!slot) {
    auto host = std::make_unique<HostTexture>();
    host->width = width;
    host->height = height;
    host->format = format;
    host->texture = g_device->createTexture(
        RenderTextureDesc::Texture2D(width, height, 1, format, RenderTextureFlag::RENDER_TARGET));
    SetDescriptor(*host);
    REXLOG_INFO("video: resolve texture {:#x} {}x{} format {}", address, width, height,
                uint32_t(fetch.format));
    slot = std::move(host);
  }
  return slot.get();
}

xenos::xe_gpu_texture_fetch_t ReadTextureFetch(uint32_t texture) {
  xenos::xe_gpu_texture_fetch_t fetch;
  uint32_t* dwords = &fetch.dword_0;
  for (uint32_t i = 0; i < 6; i++) {
    dwords[i] = guest::Load32(texture + guest::kTextureFetchConstant + i * 4);
  }
  return fetch;
}

// Present ---------------------------------------------------------------------

void Present(HostTexture* frontBuffer) {
  auto* commandList = CommandList();

  g_swapChainValid &= !g_swapChain->needsResize();
  if (!g_swapChainValid) {
    // Finish what was recorded, then rebuild the swapchain.
    commandList->end();
    g_queue->executeCommandLists(commandList, g_commandFences[g_frame].get());
    g_commandListPending[g_frame] = true;
    WaitForGPU();
    g_swapChainFramebuffers.clear();
    g_swapChainValid = g_swapChain->resize();
    commandList->begin();
  }

  uint32_t index = 0;
  if (g_swapChainValid) {
    g_swapChainValid = g_swapChain->acquireTexture(g_acquireSemaphores[g_frame].get(), &index);
  }
  while (g_renderSemaphores.size() < g_swapChain->getTextureCount()) {
    g_renderSemaphores.push_back(g_device->createCommandSemaphore());
  }

  if (g_swapChainValid) {
    if (frontBuffer) {
      Transition(*frontBuffer, RenderTextureLayout::SHADER_READ);
    }
    g_swapChainFramebuffers.resize(g_swapChain->getTextureCount());
    RenderTexture* target = g_swapChain->getTexture(index);
    auto& framebuffer = g_swapChainFramebuffers[index];
    if (!framebuffer) {
      const RenderTexture* attachments[] = {target};
      RenderFramebufferDesc desc;
      desc.colorAttachments = attachments;
      desc.colorAttachmentsCount = 1;
      framebuffer = g_device->createFramebuffer(desc);
    }
    commandList->barriers(RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(target, RenderTextureLayout::COLOR_WRITE));
    commandList->setFramebuffer(framebuffer.get());
    commandList->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));

    if (frontBuffer && frontBuffer->descriptor) {
      // Fit the guest picture into the window, keeping its aspect ratio.
      float windowWidth = float(g_swapChain->getWidth());
      float windowHeight = float(g_swapChain->getHeight());
      float scale = std::min(windowWidth / float(frontBuffer->width),
                             windowHeight / float(frontBuffer->height));
      float width = std::floor(float(frontBuffer->width) * scale);
      float height = std::floor(float(frontBuffer->height) * scale);
      RenderViewport viewport((windowWidth - width) / 2, (windowHeight - height) / 2, width,
                              height);
      RenderRect scissor(0, 0, int32_t(windowWidth), int32_t(windowHeight));
      uint32_t constants[2] = {frontBuffer->descriptor, SAMPLER_LINEAR_CLAMP};
      commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
      commandList->setPipeline(g_presentPipeline.get());
      commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
      commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
      commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
      commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 3);
      commandList->setGraphicsPushConstants(0, constants, 0, sizeof(constants));
      commandList->setViewports(&viewport, 1);
      commandList->setScissors(&scissor, 1);
      commandList->drawInstanced(3, 1, 0, 0);
    }
    commandList->barriers(RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(target, RenderTextureLayout::PRESENT));
  }

  commandList->end();
  g_recording = false;

  if (g_swapChainValid) {
    const RenderCommandList* commandLists[] = {commandList};
    RenderCommandSemaphore* waitSemaphores[] = {g_acquireSemaphores[g_frame].get()};
    RenderCommandSemaphore* signalSemaphores[] = {g_renderSemaphores[index].get()};
    g_queue->executeCommandLists(commandLists, 1, waitSemaphores, 1, signalSemaphores, 1,
                                 g_commandFences[g_frame].get());
    g_commandListPending[g_frame] = true;
    g_swapChainValid = g_swapChain->present(index, signalSemaphores, 1);
  } else {
    g_queue->executeCommandLists(commandList, g_commandFences[g_frame].get());
    g_commandListPending[g_frame] = true;
  }

  g_frame = (g_frame + 1) % NUM_FRAMES;
}

// Setup -----------------------------------------------------------------------

bool GetRenderWindow(RenderWindow& renderWindow) {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  if (!window) {
    REXLOG_ERROR("video: no SDL window");
    return false;
  }
#if defined(__linux__)
  SDL_PropertiesID props = SDL_GetWindowProperties(window);
  if (auto* surface = static_cast<wl_surface*>(
          SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr))) {
    renderWindow.waylandSurface = surface;
    renderWindow.waylandDisplay = static_cast<wl_display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    renderWindow.userData = window;
    renderWindow.getSize = [](void* userData, uint32_t* width, uint32_t* height) {
      int w = 0, h = 0;
      SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(userData), &w, &h);
      *width = uint32_t(w);
      *height = uint32_t(h);
    };
    REXLOG_INFO("video: Wayland window");
    return true;
  }
  renderWindow.display = static_cast<Display*>(
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
  renderWindow.window =
      static_cast<Window>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  if (!renderWindow.display || !renderWindow.window) {
    REXLOG_ERROR("video: unsupported SDL video driver '{}'",
                 SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
    return false;
  }
  REXLOG_INFO("video: X11 window");
  return true;
#else
  REXLOG_ERROR("video: unsupported platform");
  return false;
#endif
}

std::unique_ptr<RenderShader> CreateShader(const uint8_t* data, size_t size) {
  return g_device->createShader(data, size, "main", RenderShaderFormat::SPIRV);
}

void CreatePipelines() {
  // Texture heap bound to sets 0-2 (2D, 3D, cube views), sampler heap to set
  // 3, push constants: three buffer addresses (vertex, pixel, shared).
  RenderDescriptorSetBuilder textureSetBuilder;
  textureSetBuilder.begin();
  textureSetBuilder.addTexture(0, TEXTURE_DESCRIPTOR_COUNT);
  textureSetBuilder.end(true, TEXTURE_DESCRIPTOR_COUNT);
  g_textureDescriptorSet = textureSetBuilder.create(g_device.get());

  RenderDescriptorSetBuilder samplerSetBuilder;
  samplerSetBuilder.begin();
  samplerSetBuilder.addSampler(0, SAMPLER_DESCRIPTOR_COUNT);
  samplerSetBuilder.end(true, SAMPLER_DESCRIPTOR_COUNT);
  g_samplerDescriptorSet = samplerSetBuilder.create(g_device.get());

  RenderPipelineLayoutBuilder layoutBuilder;
  layoutBuilder.begin(false, true);
  layoutBuilder.addDescriptorSet(textureSetBuilder);
  layoutBuilder.addDescriptorSet(textureSetBuilder);
  layoutBuilder.addDescriptorSet(textureSetBuilder);
  layoutBuilder.addDescriptorSet(samplerSetBuilder);
  layoutBuilder.addPushConstant(0, 4, PUSH_CONSTANT_SIZE,
                                RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
  layoutBuilder.end();
  g_pipelineLayout = layoutBuilder.create(g_device.get());

  // Blank textures for unbound slots.
  for (uint32_t i = 0; i < DESCRIPTOR_FIRST_FREE; i++) {
    RenderTextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.format = RenderFormat::R8_UNORM;
    RenderTextureViewDesc viewDesc;
    viewDesc.format = desc.format;
    viewDesc.componentMapping = RenderComponentMapping(RenderSwizzle::ZERO, RenderSwizzle::ZERO,
                                                       RenderSwizzle::ZERO, RenderSwizzle::ZERO);
    viewDesc.mipLevels = 1;
    if (i == DESCRIPTOR_NULL_3D) {
      desc.dimension = RenderTextureDimension::TEXTURE_3D;
      desc.arraySize = 1;
      viewDesc.dimension = RenderTextureViewDimension::TEXTURE_3D;
    } else if (i == DESCRIPTOR_NULL_CUBE) {
      desc.dimension = RenderTextureDimension::TEXTURE_2D;
      desc.arraySize = 6;
      desc.flags = RenderTextureFlag::CUBE;
      viewDesc.dimension = RenderTextureViewDimension::TEXTURE_CUBE;
    } else {
      desc.dimension = RenderTextureDimension::TEXTURE_2D;
      desc.arraySize = 1;
      viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
    }
    g_nullTextures[i].texture = g_device->createTexture(desc);
    g_nullTextureViews[i] = g_nullTextures[i].texture->createTextureView(viewDesc);
    g_textureDescriptorSet->setTexture(i, g_nullTextures[i].texture.get(),
                                       RenderTextureLayout::SHADER_READ,
                                       g_nullTextureViews[i].get());
  }

  RenderSamplerDesc samplerDesc;
  samplerDesc.minFilter = RenderFilter::LINEAR;
  samplerDesc.magFilter = RenderFilter::LINEAR;
  samplerDesc.addressU = RenderTextureAddressMode::CLAMP;
  samplerDesc.addressV = RenderTextureAddressMode::CLAMP;
  samplerDesc.addressW = RenderTextureAddressMode::CLAMP;
  g_linearClampSampler = g_device->createSampler(samplerDesc);
  g_samplerDescriptorSet->setSampler(SAMPLER_LINEAR_CLAMP, g_linearClampSampler.get());

  g_fullscreenShader = CreateShader(g_fullscreen_vs, g_fullscreen_vs_size);
  g_presentShader = CreateShader(g_present_ps, g_present_ps_size);
  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = g_pipelineLayout.get();
  desc.vertexShader = g_fullscreenShader.get();
  desc.pixelShader = g_presentShader.get();
  desc.renderTargetFormat[0] = BACKBUFFER_FORMAT;
  desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
  desc.renderTargetCount = 1;
  g_presentPipeline = g_device->createGraphicsPipeline(desc);
}

}  // namespace

bool IsNativeRenderer() {
  static const bool native = [] {
    const char* renderer = std::getenv("NG2_RENDERER");
    return renderer && std::strcmp(renderer, "native") == 0;
  }();
  return native;
}

bool CreateHostDevice() {
  std::lock_guard lock(g_mutex);
  RenderWindow renderWindow{};
  if (!GetRenderWindow(renderWindow)) {
    return false;
  }

  g_interface = CreateVulkanInterface();
  if (!g_interface) {
    REXLOG_ERROR("video: Vulkan is not available");
    return false;
  }
  g_device = g_interface->createDevice();
  if (!g_device) {
    REXLOG_ERROR("video: no usable Vulkan device");
    return false;
  }
  REXLOG_INFO("video: Vulkan device '{}'", g_device->getDescription().name);

  g_queue = g_device->createCommandQueue(RenderCommandListType::DIRECT);
  for (size_t i = 0; i < NUM_FRAMES; i++) {
    g_commandLists[i] = g_queue->createCommandList();
    g_commandFences[i] = g_device->createCommandFence();
    g_acquireSemaphores[i] = g_device->createCommandSemaphore();
  }

  g_swapChain = g_queue->createSwapChain(renderWindow, 3, BACKBUFFER_FORMAT, 1);
  // plume always returns an object; one that failed (no surface, no present
  // support, format not offered) has no images.
  if (!g_swapChain || g_swapChain->getTextureCount() == 0) {
    REXLOG_ERROR("video: cannot create the swapchain");
    return false;
  }
  g_swapChain->setVsyncEnabled(true);
  g_swapChainValid = !g_swapChain->needsResize();

  CreatePipelines();
  return true;
}

void DestroyHostDevice() {
  std::lock_guard lock(g_mutex);
  if (g_queue) {
    if (g_recording) {
      g_commandLists[g_frame]->end();
      g_recording = false;
    }
    WaitForGPU();
  }
  g_framebuffers.clear();
  g_surfaces.clear();
  g_resolvedTextures.clear();
  g_presentPipeline.reset();
  g_presentShader.reset();
  g_fullscreenShader.reset();
  for (auto& view : g_nullTextureViews) {
    view.reset();
  }
  for (auto& texture : g_nullTextures) {
    texture.texture.reset();
  }
  g_linearClampSampler.reset();
  g_pipelineLayout.reset();
  g_samplerDescriptorSet.reset();
  g_textureDescriptorSet.reset();
  g_swapChainFramebuffers.clear();
  g_swapChain.reset();
  for (size_t i = 0; i < NUM_FRAMES; i++) {
    g_commandLists[i].reset();
    g_commandFences[i].reset();
    g_acquireSemaphores[i].reset();
  }
  g_renderSemaphores.clear();
  g_queue.reset();
  g_device.reset();
  g_interface.reset();
}

// D3D hooks -------------------------------------------------------------------

void OnClear(uint32_t device, uint32_t rectCount, const int32_t (*rects)[4], uint32_t flags,
             uint32_t color, float z, uint32_t stencil) {
  std::lock_guard lock(g_mutex);
  if (!g_device) {
    return;
  }
  HostTexture* target = GetSurface(guest::Load32(device + guest::device::kRenderTargets), false);
  HostTexture* depth = GetSurface(guest::Load32(device + guest::device::kDepthStencil), true);
  // 360 flags: 0x1-0x8 render targets 0-3, 0x10 Z, 0x20 stencil.
  bool clearColor = (flags & 0x1) && target;
  bool clearDepth = (flags & 0x10) && depth;
  bool clearStencil = (flags & 0x20) && depth;
  if (!clearColor && !clearDepth && !clearStencil) {
    return;
  }
  // Attachments of different shapes can't share a framebuffer.
  bool together = target && depth && target->width == depth->width &&
                  target->height == depth->height && target->samples == depth->samples;

  // Rects clipped to the target; with rects given but none usable, nothing
  // to clear.
  int32_t limitX = int32_t(target ? target->width : depth->width);
  int32_t limitY = int32_t(target ? target->height : depth->height);
  std::vector<RenderRect> clearRects;
  for (uint32_t i = 0; i < rectCount; i++) {
    RenderRect rect(std::max(rects[i][0], 0), std::max(rects[i][1], 0),
                    std::min(rects[i][2], limitX), std::min(rects[i][3], limitY));
    if (rect.right > rect.left && rect.bottom > rect.top) {
      clearRects.push_back(rect);
    }
  }
  if (rectCount && clearRects.empty()) {
    return;
  }
  const RenderRect* rectData = clearRects.empty() ? nullptr : clearRects.data();
  uint32_t rectDataCount = uint32_t(clearRects.size());

  if (clearColor) {
    BindFramebuffer(target, together ? depth : nullptr);
    RenderColor value(float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f,
                      float(color & 0xFF) / 255.0f, float(color >> 24) / 255.0f);
    CommandList()->clearColor(0, value, rectData, rectDataCount);
  }
  if (clearDepth || clearStencil) {
    if (!(clearColor && together)) {
      BindFramebuffer(nullptr, depth);
    }
    CommandList()->clearDepthStencil(clearDepth, clearStencil, z, stencil & 0xFF, rectData,
                                     rectDataCount);
  }
}

void OnResolve(uint32_t device, uint32_t flags, uint32_t sourceRect, uint32_t destTexture,
               uint32_t destPoint, uint32_t clearColor, float clearZ, uint32_t clearStencil) {
  std::lock_guard lock(g_mutex);
  if (!g_device) {
    return;
  }
  uint32_t source = flags & 7;
  bool depth = source == 4;
  uint32_t surface = depth ? guest::Load32(device + guest::device::kDepthStencil)
                           : guest::Load32(device + guest::device::kRenderTargets + source * 4);
  HostTexture* target = GetSurface(surface, depth);

  if (target && destTexture && !depth) {
    auto fetch = ReadTextureFetch(destTexture);
    uint32_t address = uint32_t(fetch.base_address) << 12;
    HostTexture* dest = GetResolveDestination(address, fetch);
    if (dest && dest->format == target->format) {
      int32_t x1 = 0, y1 = 0, x2 = int32_t(target->width), y2 = int32_t(target->height);
      if (sourceRect) {
        x1 = guest::LoadS32(sourceRect);
        y1 = guest::LoadS32(sourceRect + 4);
        x2 = guest::LoadS32(sourceRect + 8);
        y2 = guest::LoadS32(sourceRect + 12);
      }
      uint32_t dx = destPoint ? guest::Load32(destPoint) : 0;
      uint32_t dy = destPoint ? guest::Load32(destPoint + 4) : 0;
      if (dx < dest->width && dy < dest->height) {
        x1 = std::max(x1, 0);
        y1 = std::max(y1, 0);
        x2 = std::min({x2, int32_t(target->width), x1 + int32_t(dest->width - dx)});
        y2 = std::min({y2, int32_t(target->height), y1 + int32_t(dest->height - dy)});
      } else {
        x2 = x1;
      }
      if (x2 > x1 && y2 > y1) {
        if (target->samples != RenderSampleCount::COUNT_1) {
          Transition(*target, RenderTextureLayout::RESOLVE_SOURCE);
          Transition(*dest, RenderTextureLayout::RESOLVE_DEST);
          RenderRect rect(x1, y1, x2, y2);
          CommandList()->resolveTextureRegion(dest->texture.get(), dx, dy, target->texture.get(),
                                              &rect);
        } else {
          Transition(*target, RenderTextureLayout::COPY_SOURCE);
          Transition(*dest, RenderTextureLayout::COPY_DEST);
          RenderBox box(x1, y1, x2, y2);
          CommandList()->copyTextureRegion(
              RenderTextureCopyLocation::Subresource(dest->texture.get()),
              RenderTextureCopyLocation::Subresource(target->texture.get()), dx, dy, 0, &box);
        }
      }
    } else if (dest) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        REXLOG_WARN("video: resolve between different formats not done yet");
      }
    }
  }

  // Optional clears after the copy: 0x100 render target, 0x200 depth/stencil.
  if ((flags & 0x100) && target && !depth) {
    BindFramebuffer(target, nullptr);
    float rgba[4] = {};
    for (uint32_t i = 0; clearColor && i < 4; i++) {
      rgba[i] = guest::LoadFloat(clearColor + i * 4);
    }
    CommandList()->clearColor(0, RenderColor(rgba[0], rgba[1], rgba[2], rgba[3]));
  }
  if (flags & 0x200) {
    HostTexture* depthTarget =
        depth ? target : GetSurface(guest::Load32(device + guest::device::kDepthStencil), true);
    if (depthTarget) {
      BindFramebuffer(nullptr, depthTarget);
      CommandList()->clearDepthStencil(true, true, clearZ, clearStencil & 0xFF);
    }
  }
}

void OnSwap(uint32_t device, uint32_t frontBuffer) {
  (void)device;
  std::lock_guard lock(g_mutex);
  if (!g_device) {
    return;
  }
  HostTexture* front = nullptr;
  if (frontBuffer) {
    auto fetch = ReadTextureFetch(frontBuffer);
    auto it = g_resolvedTextures.find(uint32_t(fetch.base_address) << 12);
    if (it != g_resolvedTextures.end()) {
      front = it->second.get();
    }
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("video: front buffer {:#x} {}", uint32_t(fetch.base_address) << 12,
                  front ? "found" : "not resolved");
    }
  }
  Present(front);
}

}  // namespace ng2::video
