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

#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <zstd.h>

#include <plume_render_interface.h>
#include <smolv.h>

#include "guest.h"
#include "shader_cache.h"

extern const uint8_t g_fullscreen_vs[];
extern const size_t g_fullscreen_vs_size;
extern const uint8_t g_present_ps[];
extern const size_t g_present_ps_size;
extern const uint8_t g_rect_list_gs[];
extern const size_t g_rect_list_gs_size;
extern const uint8_t g_copy_ps[];
extern const size_t g_copy_ps_size;

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

using namespace plume;
namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;

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
std::unique_ptr<RenderShader> g_rectListShader;
std::unique_ptr<RenderShader> g_copyShader;
std::unordered_map<RenderFormat, std::unique_ptr<RenderPipeline>> g_copyPipelines;
// Single-sample copies of MSAA render targets, for converting resolves.
std::unordered_map<const void*, std::unique_ptr<HostTexture>> g_resolveTemps;

// Guest physical memory as the GPU sees it (512 MB): vertex data read by the
// shaders and memory export targets, addressed like the console.
constexpr uint64_t GUEST_MEMORY_SIZE = 512ull << 20;
std::unique_ptr<RenderBuffer> g_guestMemory;
uint64_t g_guestMemoryAddress = 0;
// Ranges uploaded to it and the hash of their contents then.
std::unordered_map<uint64_t, uint64_t> g_guestMemoryHashes;

// Per-frame upload memory for constants and copies.
constexpr uint64_t UPLOAD_CHUNK_SIZE = 32ull << 20;
struct UploadChunk {
  std::unique_ptr<RenderBuffer> buffer;
  uint64_t size = 0;
  uint8_t* data = nullptr;
  uint64_t address = 0;
};
struct UploadAllocator {
  std::vector<UploadChunk> chunks;
  size_t chunk = 0;
  uint64_t offset = 0;
  void Reset() {
    chunk = 0;
    offset = 0;
  }
};
UploadAllocator g_uploads[NUM_FRAMES];
struct Upload {
  RenderBuffer* buffer;
  uint64_t offset;
  uint8_t* data;
  uint64_t address;
};

// Shaders created by the game, by guest object.
struct GuestShader {
  uint32_t container = 0;
  const ShaderCacheEntry* entry = nullptr;
  // Vertex shaders: usage and usage index of each declared element slot.
  std::vector<std::pair<uint8_t, uint8_t>> elements;
};
std::unordered_map<uint32_t, std::unique_ptr<GuestShader>> g_guestShaders;
std::unique_ptr<uint8_t[]> g_spirvCache;
// Host shaders by cache entry: they live as long as the device, so pipelines
// can refer to them.
std::unordered_map<const ShaderCacheEntry*, std::unique_ptr<RenderShader>> g_hostShaders;

std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> g_pipelines;

// Guest textures by fetch constant, with the hash of their memory when last
// uploaded (checked once per frame).
struct CachedTexture {
  std::unique_ptr<HostTexture> host;
  uint64_t contentHash = 0;
  uint64_t checkedFrame = ~0ull;
};
std::unordered_map<uint64_t, CachedTexture> g_textures;
// Host textures replaced while a frame may still use them: freed once that
// frame's fence has passed.
std::vector<std::unique_ptr<HostTexture>> g_garbage[NUM_FRAMES];
uint64_t g_frameCount = 0;

// NG2_TRACE_FRAME=<n>: log what frame n draws.
bool Tracing() {
  static const int64_t frame = [] {
    const char* v = std::getenv("NG2_TRACE_FRAME");
    return v ? std::strtoll(v, nullptr, 10) : -1ll;
  }();
  return int64_t(g_frameCount) == frame;
}

std::unordered_map<uint64_t, std::pair<uint32_t, std::unique_ptr<RenderSampler>>> g_samplers;
uint32_t g_nextSampler = SAMPLER_FIRST_FREE;

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

// The framebuffer last set on the command list (plume resumes it after
// barriers and copies end the render pass).
const RenderFramebuffer* g_boundFramebuffer = nullptr;

// Drops cached framebuffers that use a texture about to be freed.
void ForgetTexture(const HostTexture* texture) {
  for (auto it = g_framebuffers.begin(); it != g_framebuffers.end();) {
    if (it->first.color == texture || it->first.depth == texture) {
      if (g_boundFramebuffer == it->second.get()) {
        g_boundFramebuffer = nullptr;
      }
      it = g_framebuffers.erase(it);
    } else {
      ++it;
    }
  }
}


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
    g_boundFramebuffer = nullptr;
    g_uploads[g_frame].Reset();
    for (auto& texture : g_garbage[g_frame]) {
      g_descriptors.Free(texture->descriptor);
      ForgetTexture(texture.get());
    }
    g_garbage[g_frame].clear();
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
  if (g_boundFramebuffer != framebuffer.get()) {
    CommandList()->setFramebuffer(framebuffer.get());
    g_boundFramebuffer = framebuffer.get();
  }
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
    ForgetTexture(slot.get());
    g_resolveTemps.erase(slot.get());
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

// Texture objects keep CPU virtual addresses in their fetch constant; the
// device's fetch constants (SetTexture) hold physical ones. Same conversion
// as D3D's (docs/d3d-map-c.md).
uint32_t PhysicalAddress(uint32_t address) {
  return (address & 0x1FFFFFFF) + (address >= 0xE0000000 ? 0x1000 : 0);
}

xenos::xe_gpu_texture_fetch_t ReadTextureFetch(uint32_t texture) {
  xenos::xe_gpu_texture_fetch_t fetch;
  uint32_t* dwords = &fetch.dword_0;
  for (uint32_t i = 0; i < 6; i++) {
    dwords[i] = guest::Load32(texture + guest::kTextureFetchConstant + i * 4);
  }
  return fetch;
}



// Copies [x1, y1, x2, y2) of a render target to (dx, dy) in a resolve
// destination, converting the format with a draw.
void ResolveByDraw(HostTexture& source, HostTexture& dest, int32_t x1, int32_t y1, int32_t x2,
                   int32_t y2, uint32_t dx, uint32_t dy) {
  HostTexture* readable = &source;
  if (source.samples != RenderSampleCount::COUNT_1) {
    auto& temp = g_resolveTemps[&source];
    if (!temp) {
      temp = std::make_unique<HostTexture>();
      temp->width = source.width;
      temp->height = source.height;
      temp->format = source.format;
      temp->texture = g_device->createTexture(RenderTextureDesc::Texture2D(
          source.width, source.height, 1, source.format, RenderTextureFlag::RENDER_TARGET));
    }
    Transition(source, RenderTextureLayout::RESOLVE_SOURCE);
    Transition(*temp, RenderTextureLayout::RESOLVE_DEST);
    RenderRect rect(x1, y1, x2, y2);
    CommandList()->resolveTextureRegion(temp->texture.get(), x1, y1, source.texture.get(), &rect);
    readable = temp.get();
  }
  SetDescriptor(*readable);
  if (!readable->descriptor) {
    return;
  }
  Transition(*readable, RenderTextureLayout::SHADER_READ);

  auto& pipeline = g_copyPipelines[dest.format];
  if (!pipeline) {
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = g_pipelineLayout.get();
    desc.vertexShader = g_fullscreenShader.get();
    desc.pixelShader = g_copyShader.get();
    desc.renderTargetFormat[0] = dest.format;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    pipeline = g_device->createGraphicsPipeline(desc);
  }

  BindFramebuffer(&dest, nullptr);
  auto* commandList = CommandList();
  RenderViewport viewport(float(dx), float(dy), float(x2 - x1), float(y2 - y1));
  RenderRect scissor(int32_t(dx), int32_t(dy), int32_t(dx) + (x2 - x1), int32_t(dy) + (y2 - y1));
  int32_t constants[3] = {int32_t(readable->descriptor), x1 - int32_t(dx), y1 - int32_t(dy)};
  commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
  commandList->setPipeline(pipeline.get());
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
  commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 3);
  commandList->setGraphicsPushConstants(0, constants, 0, sizeof(constants));
  commandList->setViewports(&viewport, 1);
  commandList->setScissors(&scissor, 1);
  commandList->drawInstanced(3, 1, 0, 0);
}

// Uploads ----------------------------------------------------------------------

Upload Allocate(uint64_t size, uint64_t alignment = 256) {
  auto& allocator = g_uploads[g_frame];
  allocator.offset = (allocator.offset + alignment - 1) & ~(alignment - 1);
  // Move on while the current chunk can't hold it (chunks can be bigger
  // than UPLOAD_CHUNK_SIZE for big uploads).
  while (allocator.chunk < allocator.chunks.size() &&
         allocator.offset + size > allocator.chunks[allocator.chunk].size) {
    allocator.chunk++;
    allocator.offset = 0;
  }
  if (allocator.chunk >= allocator.chunks.size()) {
    UploadChunk chunk;
    chunk.size = std::max(UPLOAD_CHUNK_SIZE, size);
    chunk.buffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(
        chunk.size,
        RenderBufferFlag::CONSTANT | RenderBufferFlag::STORAGE | RenderBufferFlag::INDEX));
    chunk.data = static_cast<uint8_t*>(chunk.buffer->map());
    chunk.address = chunk.buffer->getDeviceAddress();
    allocator.chunks.push_back(std::move(chunk));
  }
  auto& chunk = allocator.chunks[allocator.chunk];
  Upload upload{chunk.buffer.get(), allocator.offset, chunk.data + allocator.offset,
                chunk.address + allocator.offset};
  allocator.offset += size;
  return upload;
}

// Makes guest physical [address, address + size) current in g_guestMemory.
void SyncGuestMemory(uint32_t address, uint32_t size) {
  if (!size || uint64_t(address) + size > GUEST_MEMORY_SIZE) {
    return;
  }
  const uint8_t* source = guest::Physical(address);
  uint64_t hash = XXH3_64bits(source, size);
  uint64_t key = (uint64_t(address) << 32) | size;
  auto it = g_guestMemoryHashes.find(key);
  if (it != g_guestMemoryHashes.end() && it->second == hash) {
    return;
  }
  g_guestMemoryHashes[key] = hash;
  Upload upload = Allocate(size, 16);
  std::memcpy(upload.data, source, size);
  auto* commandList = CommandList();
  commandList->barriers(RenderBarrierStage::COPY,
                        RenderBufferBarrier(g_guestMemory.get(), RenderBufferAccess::WRITE));
  commandList->copyBufferRegion(RenderBufferReference(g_guestMemory.get(), address),
                                RenderBufferReference(upload.buffer, upload.offset), size);
  commandList->barriers(RenderBarrierStage::GRAPHICS,
                        RenderBufferBarrier(g_guestMemory.get(),
                                            RenderBufferAccess::READ | RenderBufferAccess::WRITE));
}

// Shaders ---------------------------------------------------------------------

const ShaderCacheEntry* FindShaderCacheEntry(uint64_t hash) {
  auto* begin = g_shaderCacheEntries;
  auto* end = g_shaderCacheEntries + g_shaderCacheEntryCount;
  auto* it = std::lower_bound(begin, end, hash, [](const ShaderCacheEntry& entry,
                                                   uint64_t value) { return entry.hash < value; });
  return (it != end && it->hash == hash) ? it : nullptr;
}

RenderShader* GetHostShader(GuestShader& guestShader) {
  if (!guestShader.entry) {
    return nullptr;
  }
  auto& host = g_hostShaders[guestShader.entry];
  if (!host) {
    if (!g_spirvCache) {
      g_spirvCache = std::make_unique<uint8_t[]>(g_spirvCacheDecompressedSize);
      ZSTD_decompress(g_spirvCache.get(), g_spirvCacheDecompressedSize, g_compressedSpirvCache,
                      g_spirvCacheCompressedSize);
    }
    const uint8_t* encoded = g_spirvCache.get() + guestShader.entry->spirvOffset;
    size_t size = smolv::GetDecodedBufferSize(encoded, guestShader.entry->spirvSize);
    std::vector<uint8_t> spirv(size);
    if (smolv::Decode(encoded, guestShader.entry->spirvSize, spirv.data(), size)) {
      host = g_device->createShader(spirv.data(), size, "main", RenderShaderFormat::SPIRV);
    }
  }
  return host.get();
}


// Textures --------------------------------------------------------------------

struct TextureFormatInfo {
  RenderFormat format = RenderFormat::UNKNOWN;
  uint32_t blockSize = 1;      // texels per block side
  uint32_t bytesPerBlock = 0;
};

TextureFormatInfo GetTextureFormatInfo(xenos::TextureFormat format) {
  using F = xenos::TextureFormat;
  switch (format) {
    case F::k_8: return {RenderFormat::R8_UNORM, 1, 1};
    case F::k_8_8: return {RenderFormat::R8G8_UNORM, 1, 2};
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_AS_16_16_16_16: return {RenderFormat::R8G8B8A8_UNORM, 1, 4};
    case F::k_16: return {RenderFormat::R16_UNORM, 1, 2};
    case F::k_16_16: return {RenderFormat::R16G16_UNORM, 1, 4};
    case F::k_16_16_16_16: return {RenderFormat::R16G16B16A16_UNORM, 1, 8};
    case F::k_16_FLOAT: return {RenderFormat::R16_FLOAT, 1, 2};
    case F::k_16_16_FLOAT: return {RenderFormat::R16G16_FLOAT, 1, 4};
    case F::k_16_16_16_16_FLOAT: return {RenderFormat::R16G16B16A16_FLOAT, 1, 8};
    case F::k_32_FLOAT: return {RenderFormat::R32_FLOAT, 1, 4};
    case F::k_32_32_FLOAT: return {RenderFormat::R32G32_FLOAT, 1, 8};
    case F::k_32_32_32_32_FLOAT: return {RenderFormat::R32G32B32A32_FLOAT, 1, 16};
    case F::k_DXT1: return {RenderFormat::BC1_UNORM, 4, 8};
    case F::k_DXT2_3: return {RenderFormat::BC2_UNORM, 4, 16};
    case F::k_DXT4_5: return {RenderFormat::BC3_UNORM, 4, 16};
    case F::k_DXN: return {RenderFormat::BC5_UNORM, 4, 16};
    case F::k_DXT5A: return {RenderFormat::BC4_UNORM, 4, 8};
    default: return {};
  }
}

// Address of block (x, y) in a tiled 2D texture, from Xenia
// (texture_util.cc, after UModel's UnTexture.cpp).
int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytesPerBlockLog2) {
  pitch = (pitch + 31) & ~31u;
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bytesPerBlockLog2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bytesPerBlockLog2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

void EndianSwap(uint8_t* data, size_t size, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      for (size_t i = 0; i + 1 < size; i += 2) {
        std::swap(data[i], data[i + 1]);
      }
      break;
    case xenos::Endian::k8in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 3]);
        std::swap(data[i + 1], data[i + 2]);
      }
      break;
    case xenos::Endian::k16in32:
      for (size_t i = 0; i + 3 < size; i += 4) {
        std::swap(data[i], data[i + 2]);
        std::swap(data[i + 1], data[i + 3]);
      }
      break;
    default:
      break;
  }
}

RenderSwizzle ConvertSwizzle(uint32_t component) {
  switch (component) {
    case 0: return RenderSwizzle::R;
    case 1: return RenderSwizzle::G;
    case 2: return RenderSwizzle::B;
    case 3: return RenderSwizzle::A;
    case 4: return RenderSwizzle::ZERO;
    default: return RenderSwizzle::ONE;
  }
}

RenderTextureAddressMode ConvertClamp(xenos::ClampMode mode) {
  switch (mode) {
    case xenos::ClampMode::kRepeat: return RenderTextureAddressMode::WRAP;
    case xenos::ClampMode::kMirroredRepeat: return RenderTextureAddressMode::MIRROR;
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway: return RenderTextureAddressMode::MIRROR_ONCE;
    case xenos::ClampMode::kClampToBorder:
    case xenos::ClampMode::kMirrorClampToBorder: return RenderTextureAddressMode::BORDER;
    default: return RenderTextureAddressMode::CLAMP;
  }
}

uint32_t GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch) {
  RenderSamplerDesc desc;
  desc.addressU = ConvertClamp(fetch.clamp_x);
  desc.addressV = ConvertClamp(fetch.clamp_y);
  desc.addressW = ConvertClamp(fetch.clamp_z);
  desc.magFilter = fetch.mag_filter == xenos::TextureFilter::kPoint ? RenderFilter::NEAREST
                                                                    : RenderFilter::LINEAR;
  desc.minFilter = fetch.min_filter == xenos::TextureFilter::kPoint ? RenderFilter::NEAREST
                                                                    : RenderFilter::LINEAR;
  desc.mipmapMode = fetch.mip_filter == xenos::TextureFilter::kLinear ? RenderMipmapMode::LINEAR
                                                                       : RenderMipmapMode::NEAREST;
  desc.maxLOD = 0.0f;  // base level only for now
  uint64_t hash = XXH3_64bits(&desc, sizeof(desc));
  auto& sampler = g_samplers[hash];
  if (!sampler.second) {
    if (g_nextSampler >= SAMPLER_DESCRIPTOR_COUNT) {
      return SAMPLER_LINEAR_CLAMP;
    }
    sampler.first = g_nextSampler++;
    sampler.second = g_device->createSampler(desc);
    g_samplerDescriptorSet->setSampler(sampler.first, sampler.second.get());
  }
  return sampler.first;
}

// Uploads the base level of a guest texture: endian swap, untile.
std::unique_ptr<HostTexture> CreateTexture(const xenos::xe_gpu_texture_fetch_t& fetch,
                                           const TextureFormatInfo& info, uint32_t width,
                                           uint32_t height, const uint8_t* guestData,
                                           uint32_t guestSize) {
  uint32_t blocksX = (width + info.blockSize - 1) / info.blockSize;
  uint32_t blocksY = (height + info.blockSize - 1) / info.blockSize;
  uint32_t pitchBlocks = std::max((uint32_t(fetch.pitch) << 5) / info.blockSize, blocksX);
  uint32_t bppLog2 = uint32_t(std::countr_zero(info.bytesPerBlock));

  std::vector<uint8_t> swapped(guestData, guestData + guestSize);
  EndianSwap(swapped.data(), swapped.size(), fetch.endianness);

  uint64_t rowBytes = uint64_t(blocksX) * info.bytesPerBlock;
  Upload upload = Allocate(rowBytes * blocksY, 16);
  uint64_t linearRowBytes = (uint64_t(pitchBlocks) * info.bytesPerBlock + 255) & ~255ull;
  for (uint32_t y = 0; y < blocksY; y++) {
    uint8_t* row = upload.data + y * rowBytes;
    if (fetch.tiled) {
      for (uint32_t x = 0; x < blocksX; x++) {
        int32_t offset = TiledOffset2D(int32_t(x), int32_t(y), pitchBlocks, bppLog2);
        if (offset >= 0 && uint64_t(offset) + info.bytesPerBlock <= guestSize) {
          std::memcpy(row + x * info.bytesPerBlock, swapped.data() + offset, info.bytesPerBlock);
        }
      }
    } else if ((y + 1) * linearRowBytes <= guestSize) {
      std::memcpy(row, swapped.data() + y * linearRowBytes, rowBytes);
    }
  }

  // NG2_DUMP_TEXTURES=<dir>: write decoded 8-bit and 8888 textures (PGM/PAM).
  static const char* dumpDir = std::getenv("NG2_DUMP_TEXTURES");
  if (dumpDir && (info.format == RenderFormat::R8_UNORM ||
                  info.format == RenderFormat::R8G8B8A8_UNORM)) {
    bool gray = info.format == RenderFormat::R8_UNORM;
    std::string path = fmt::format("{}/{:08x}_{}x{}.{}", dumpDir,
                                   uint32_t(fetch.base_address) << 12, width, height,
                                   gray ? "pgm" : "pam");
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      if (gray) {
        std::fprintf(f, "P5\n%u %u\n255\n", width, height);
      } else {
        std::fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
                     width, height);
      }
      for (uint32_t y = 0; y < height; y++) {
        std::fwrite(upload.data + y * rowBytes, 1, width * info.bytesPerBlock, f);
      }
      std::fclose(f);
    }
  }

  auto host = std::make_unique<HostTexture>();
  host->width = width;
  host->height = height;
  host->format = info.format;
  host->texture =
      g_device->createTexture(RenderTextureDesc::Texture2D(width, height, 1, info.format));
  Transition(*host, RenderTextureLayout::COPY_DEST);
  CommandList()->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(host->texture.get()),
      RenderTextureCopyLocation::PlacedFootprint(upload.buffer, info.format, width, height, 1,
                                                 blocksX * info.blockSize, upload.offset));

  RenderTextureViewDesc viewDesc;
  viewDesc.format = info.format;
  viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
  viewDesc.mipLevels = 1;
  viewDesc.componentMapping = RenderComponentMapping(
      ConvertSwizzle(fetch.swizzle & 7), ConvertSwizzle((fetch.swizzle >> 3) & 7),
      ConvertSwizzle((fetch.swizzle >> 6) & 7), ConvertSwizzle((fetch.swizzle >> 9) & 7));
  host->view = host->texture->createTextureView(viewDesc);
  host->descriptor = g_descriptors.Allocate();
  if (host->descriptor) {
    g_textureDescriptorSet->setTexture(host->descriptor, host->texture.get(),
                                       RenderTextureLayout::SHADER_READ, host->view.get());
  }
  return host;
}

// Descriptor index for a texture fetch constant, 0 if it can't be sampled.
uint32_t GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  bool oneD = fetch.dimension == xenos::DataDimension::k1D;
  if (!oneD && fetch.dimension != xenos::DataDimension::k2DOrStacked) {
    return 0;  // 3D and cube textures: not yet
  }
  uint32_t base = uint32_t(fetch.base_address) << 12;

  // Render-to-texture results live on the GPU.
  if (auto it = g_resolvedTextures.find(base); it != g_resolvedTextures.end()) {
    Transition(*it->second, RenderTextureLayout::SHADER_READ);
    return it->second->descriptor;
  }

  TextureFormatInfo info = GetTextureFormatInfo(fetch.format);
  if (info.format == RenderFormat::UNKNOWN) {
    return 0;
  }
  uint32_t width = oneD ? fetch.size_1d.width + 1 : fetch.size_2d.width + 1;
  uint32_t height = oneD ? 1 : fetch.size_2d.height + 1;
  uint32_t blocksX = (width + info.blockSize - 1) / info.blockSize;
  uint32_t blocksY = (height + info.blockSize - 1) / info.blockSize;
  uint32_t pitchBlocks = std::max((uint32_t(fetch.pitch) << 5) / info.blockSize, blocksX);
  uint64_t guestSize =
      fetch.tiled ? uint64_t((pitchBlocks + 31) & ~31u) * ((blocksY + 31) & ~31u) * info.bytesPerBlock
                  : ((uint64_t(pitchBlocks) * info.bytesPerBlock + 255) & ~255ull) * blocksY;
  if (guestSize == 0 || base + guestSize > GUEST_MEMORY_SIZE) {
    return 0;
  }

  // Shape and view: dwords 0-2 and the swizzle; sampler fields don't matter.
  uint32_t keyData[4] = {fetch.dword_0 & ~0x3FFFFFu, fetch.dword_1, fetch.dword_2,
                         uint32_t(fetch.swizzle)};
  uint64_t key = XXH3_64bits(keyData, sizeof(keyData));
  auto& cached = g_textures[key];
  if (cached.checkedFrame != g_frameCount) {
    cached.checkedFrame = g_frameCount;
    const uint8_t* data = guest::Physical(base);
    uint64_t hash = XXH3_64bits(data, guestSize);
    if (!cached.host || hash != cached.contentHash) {
      if (cached.host) {
        g_garbage[g_frame].push_back(std::move(cached.host));
      }
      cached.host = CreateTexture(fetch, info, width, height, data, uint32_t(guestSize));
      cached.contentHash = hash;
    }
  }
  Transition(*cached.host, RenderTextureLayout::SHADER_READ);
  return cached.host->descriptor;
}

// Device state ----------------------------------------------------------------

// The device keeps GPU register shadows in blocks (docs/d3d-map-c.md).
uint32_t Register(uint32_t device, uint32_t index) {
  struct Block {
    uint32_t first, count, offset;
  };
  static constexpr Block blocks[] = {
      {0x2000, 16, 0x2880}, {0x2100, 21, 0x28CC}, {0x2180, 5, 0x2920},  {0x2200, 12, 0x2934},
      {0x2280, 21, 0x2964}, {0x2300, 38, 0x29B8}, {0x2380, 8, 0x2A50},
  };
  for (const auto& block : blocks) {
    if (index >= block.first && index < block.first + block.count) {
      return guest::Load32(device + block.offset + (index - block.first) * 4);
    }
  }
  return 0;
}

float RegisterFloat(uint32_t device, uint32_t index) {
  uint32_t bits = Register(device, index);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

RenderBlend ConvertBlend(xenos::BlendFactor factor) {
  switch (factor) {
    case xenos::BlendFactor::kZero: return RenderBlend::ZERO;
    case xenos::BlendFactor::kOne: return RenderBlend::ONE;
    case xenos::BlendFactor::kSrcColor: return RenderBlend::SRC_COLOR;
    case xenos::BlendFactor::kOneMinusSrcColor: return RenderBlend::INV_SRC_COLOR;
    case xenos::BlendFactor::kSrcAlpha: return RenderBlend::SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcAlpha: return RenderBlend::INV_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor: return RenderBlend::DEST_COLOR;
    case xenos::BlendFactor::kOneMinusDstColor: return RenderBlend::INV_DEST_COLOR;
    case xenos::BlendFactor::kDstAlpha: return RenderBlend::DEST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstAlpha: return RenderBlend::INV_DEST_ALPHA;
    case xenos::BlendFactor::kConstantColor: return RenderBlend::BLEND_FACTOR;
    case xenos::BlendFactor::kOneMinusConstantColor: return RenderBlend::INV_BLEND_FACTOR;
    case xenos::BlendFactor::kConstantAlpha: return RenderBlend::BLEND_FACTOR;
    case xenos::BlendFactor::kOneMinusConstantAlpha: return RenderBlend::INV_BLEND_FACTOR;
    case xenos::BlendFactor::kSrcAlphaSaturate: return RenderBlend::SRC_ALPHA_SAT;
  }
  return RenderBlend::ONE;
}

RenderBlendOperation ConvertBlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kAdd: return RenderBlendOperation::ADD;
    case xenos::BlendOp::kSubtract: return RenderBlendOperation::SUBTRACT;
    case xenos::BlendOp::kMin: return RenderBlendOperation::MIN;
    case xenos::BlendOp::kMax: return RenderBlendOperation::MAX;
    case xenos::BlendOp::kRevSubtract: return RenderBlendOperation::REV_SUBTRACT;
  }
  return RenderBlendOperation::ADD;
}

RenderComparisonFunction ConvertCompare(xenos::CompareFunction function) {
  switch (function) {
    case xenos::CompareFunction::kNever: return RenderComparisonFunction::NEVER;
    case xenos::CompareFunction::kLess: return RenderComparisonFunction::LESS;
    case xenos::CompareFunction::kEqual: return RenderComparisonFunction::EQUAL;
    case xenos::CompareFunction::kLessEqual: return RenderComparisonFunction::LESS_EQUAL;
    case xenos::CompareFunction::kGreater: return RenderComparisonFunction::GREATER;
    case xenos::CompareFunction::kNotEqual: return RenderComparisonFunction::NOT_EQUAL;
    case xenos::CompareFunction::kGreaterEqual: return RenderComparisonFunction::GREATER_EQUAL;
    case xenos::CompareFunction::kAlways: return RenderComparisonFunction::ALWAYS;
  }
  return RenderComparisonFunction::ALWAYS;
}

RenderStencilOp ConvertStencilOp(xenos::StencilOp op) {
  switch (op) {
    case xenos::StencilOp::kKeep: return RenderStencilOp::KEEP;
    case xenos::StencilOp::kZero: return RenderStencilOp::ZERO;
    case xenos::StencilOp::kReplace: return RenderStencilOp::REPLACE;
    case xenos::StencilOp::kIncrementClamp: return RenderStencilOp::INCREMENT_AND_CLAMP;
    case xenos::StencilOp::kDecrementClamp: return RenderStencilOp::DECREMENT_AND_CLAMP;
    case xenos::StencilOp::kInvert: return RenderStencilOp::INVERT;
    case xenos::StencilOp::kIncrementWrap: return RenderStencilOp::INCREMENT_AND_WRAP;
    case xenos::StencilOp::kDecrementWrap: return RenderStencilOp::DECREMENT_AND_WRAP;
  }
  return RenderStencilOp::KEEP;
}

// D3DDECLTYPE on the 360: bits 0-5 GPU vertex format, 8 signed, 9 integer.
uint32_t ElementFlags(uint32_t declType) {
  uint32_t format = declType & 0x3F;
  uint32_t flags = format | (declType & 0x300);
  if (declType == 0x182886) {  // D3DDECLTYPE_D3DCOLOR: BGRA in memory
    flags |= 0x800;
  }
  return flags;
}

// Shared constants, laid out as docs/shaders.md describes.
struct SharedConstants {
  uint32_t textureIndices[4][32];
  uint32_t samplerIndices[32];
  uint32_t booleans[8];
  uint32_t swappedTexcoords;
  float alphaThreshold;
  float halfPixelOffset[2];
  uint64_t guestMemoryBase;
  uint32_t vertexFetch[96][2];
  uint32_t vertexElements[16][4];
  float ndcTransform[4];
};
static_assert(offsetof(SharedConstants, samplerIndices) == 512);
static_assert(offsetof(SharedConstants, booleans) == 640);
static_assert(offsetof(SharedConstants, guestMemoryBase) == 688);
static_assert(offsetof(SharedConstants, vertexFetch) == 696);
static_assert(offsetof(SharedConstants, vertexElements) == 1464);
static_assert(offsetof(SharedConstants, ndcTransform) == 1720);

// Pipeline state that varies per draw (hashed into the pipeline key).
struct PipelineKey {
  uint64_t vertexShader;  // shader cache entry hashes
  uint64_t pixelShader;
  uint32_t primitive;
  uint32_t colorFormat;
  uint32_t depthFormat;
  uint32_t samples;
  uint32_t blendControl;
  uint32_t colorMask;
  uint32_t depthControl;
  uint32_t stencilMasks;
  uint32_t cull;
  uint32_t alphaTest;
};

RenderPipeline* GetPipeline(const PipelineKey& key, RenderShader* vertexShader,
                            RenderShader* pixelShader) {
  uint64_t hash = XXH3_64bits(&key, sizeof(key));
  auto& pipeline = g_pipelines[hash];
  if (pipeline) {
    return pipeline.get();
  }
  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = g_pipelineLayout.get();
  desc.vertexShader = vertexShader;
  desc.pixelShader = pixelShader;
  if (key.primitive == uint32_t(xenos::PrimitiveType::kRectangleList)) {
    desc.geometryShader = g_rectListShader.get();
    desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
  } else {
    switch (xenos::PrimitiveType(key.primitive)) {
      case xenos::PrimitiveType::kPointList:
        desc.primitiveTopology = RenderPrimitiveTopology::POINT_LIST;
        break;
      case xenos::PrimitiveType::kLineList:
        desc.primitiveTopology = RenderPrimitiveTopology::LINE_LIST;
        break;
      case xenos::PrimitiveType::kLineStrip:
        desc.primitiveTopology = RenderPrimitiveTopology::LINE_STRIP;
        break;
      case xenos::PrimitiveType::kTriangleFan:
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_FAN;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_STRIP;
        break;
      default:
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        break;
    }
  }
  reg::PA_SU_SC_MODE_CNTL mode;
  mode.value = key.cull;
  // D3D on the 360: front faces are clockwise unless PA_SU_SC_MODE_CNTL.face.
  if (mode.cull_front && mode.cull_back) {
    desc.cullMode = RenderCullMode::NONE;  // everything culled: drawn as nothing below
  } else if (mode.cull_front) {
    desc.cullMode = mode.face ? RenderCullMode::BACK : RenderCullMode::FRONT;
  } else if (mode.cull_back) {
    desc.cullMode = mode.face ? RenderCullMode::FRONT : RenderCullMode::BACK;
  }
  if (key.colorFormat != uint32_t(RenderFormat::UNKNOWN)) {
    desc.renderTargetFormat[0] = RenderFormat(key.colorFormat);
    desc.renderTargetCount = 1;
    reg::RB_BLENDCONTROL blend;
    blend.value = key.blendControl;
    auto& target = desc.renderTargetBlend[0];
    target.srcBlend = ConvertBlend(blend.color_srcblend);
    target.dstBlend = ConvertBlend(blend.color_destblend);
    target.blendOp = ConvertBlendOp(blend.color_comb_fcn);
    target.srcBlendAlpha = ConvertBlend(blend.alpha_srcblend);
    target.dstBlendAlpha = ConvertBlend(blend.alpha_destblend);
    target.blendOpAlpha = ConvertBlendOp(blend.alpha_comb_fcn);
    target.blendEnabled =
        !(target.srcBlend == RenderBlend::ONE && target.dstBlend == RenderBlend::ZERO &&
          target.blendOp == RenderBlendOperation::ADD &&
          target.srcBlendAlpha == RenderBlend::ONE && target.dstBlendAlpha == RenderBlend::ZERO &&
          target.blendOpAlpha == RenderBlendOperation::ADD);
    target.renderTargetWriteMask = uint8_t(key.colorMask & 0xF);
  }
  desc.depthTargetFormat = RenderFormat(key.depthFormat);
  if (desc.depthTargetFormat != RenderFormat::UNKNOWN) {
    reg::RB_DEPTHCONTROL depth;
    depth.value = key.depthControl;
    desc.depthEnabled = depth.z_enable;
    desc.depthWriteEnabled = depth.z_enable && depth.z_write_enable;
    desc.depthFunction = ConvertCompare(depth.zfunc);
    desc.stencilEnabled = depth.stencil_enable;
    desc.stencilFrontFace.compareFunction = ConvertCompare(depth.stencilfunc);
    desc.stencilFrontFace.failOp = ConvertStencilOp(depth.stencilfail);
    desc.stencilFrontFace.passOp = ConvertStencilOp(depth.stencilzpass);
    desc.stencilFrontFace.depthFailOp = ConvertStencilOp(depth.stencilzfail);
    if (depth.backface_enable) {
      desc.stencilBackFace.compareFunction = ConvertCompare(depth.stencilfunc_bf);
      desc.stencilBackFace.failOp = ConvertStencilOp(depth.stencilfail_bf);
      desc.stencilBackFace.passOp = ConvertStencilOp(depth.stencilzpass_bf);
      desc.stencilBackFace.depthFailOp = ConvertStencilOp(depth.stencilzfail_bf);
    } else {
      desc.stencilBackFace = desc.stencilFrontFace;
    }
    reg::RB_STENCILREFMASK stencil;
    stencil.value = key.stencilMasks;
    desc.stencilReadMask = stencil.stencilmask;
    desc.stencilWriteMask = stencil.stencilwritemask;
    desc.stencilReference = stencil.stencilref;
  }
  desc.multisampling.sampleCount = RenderSampleCounts(key.samples);
  RenderSpecConstant specConstant(0, key.alphaTest ? 0x2u : 0u);  // SPEC_CONSTANT_ALPHA_TEST
  desc.specConstants = &specConstant;
  desc.specConstantsCount = 1;
  pipeline = g_device->createGraphicsPipeline(desc);
  return pipeline.get();
}

// A draw with everything captured, ready to record.
struct DrawRecord {
  RenderPipeline* pipeline = nullptr;
  bool memExport = false;
  HostTexture* target = nullptr;
  HostTexture* depth = nullptr;
  RenderViewport viewport;
  RenderRect scissor;
  uint64_t pushConstants[3] = {};
  bool indexed = false;
  std::optional<RenderIndexBufferView> indexView;
  uint32_t drawCount = 0;
  uint32_t startVertex = 0;
};

// BeginVertices / BeginIndexedVertices hand the game a pointer into the
// command ring and the game writes the data after the call: the draw is
// captured at Begin and recorded at the next hooked call.
struct InlineData {
  uint32_t vertexAddress = 0;  // guest virtual
  uint32_t vertexBytes = 0;
  uint32_t stride = 0;         // bytes
  uint32_t indexAddress = 0;   // guest virtual, 0 if not indexed
  uint32_t indexCount = 0;
  bool wideIndices = false;
};
struct PendingDraw {
  DrawRecord record;
  InlineData data;
};
std::optional<PendingDraw> g_pendingDraw;

void SubmitDraw(DrawRecord& record) {
  BindFramebuffer(record.target, record.depth);
  auto* commandList = CommandList();
  commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
  commandList->setPipeline(record.pipeline);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
  commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 3);
  commandList->setGraphicsPushConstants(0, record.pushConstants, 0, sizeof(record.pushConstants));
  commandList->setViewports(&record.viewport, 1);
  commandList->setScissors(&record.scissor, 1);
  if (record.indexed) {
    commandList->setIndexBuffer(&*record.indexView);
    commandList->drawIndexedInstanced(record.drawCount, 1, 0, int32_t(record.startVertex), 0);
  } else {
    commandList->drawInstanced(record.drawCount, 1, record.startVertex, 0);
  }
  // Memory export results feed later draws.
  if (record.memExport) {
    commandList->barriers(RenderBarrierStage::GRAPHICS,
                          RenderBufferBarrier(g_guestMemory.get(),
                                              RenderBufferAccess::READ | RenderBufferAccess::WRITE));
  }
}

template <typename T>
void SwapIndices(uint8_t* out, const uint8_t* in, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    reinterpret_cast<T*>(out)[i] = rex::byte_swap(reinterpret_cast<const T*>(in)[i]);
  }
}

void FlushPendingDraw() {
  if (!g_pendingDraw) {
    return;
  }
  PendingDraw pending = std::move(*g_pendingDraw);
  g_pendingDraw.reset();
  auto& data = pending.data;
  SyncGuestMemory(PhysicalAddress(data.vertexAddress), data.vertexBytes);
  if (Tracing()) {
    const uint32_t* v = reinterpret_cast<const uint32_t*>(guest::Virtual(data.vertexAddress));
    REXLOG_INFO("  inline vertices at {:#x} ({} bytes): {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
                data.vertexAddress, data.vertexBytes, rex::byte_swap(v[0]), rex::byte_swap(v[1]),
                rex::byte_swap(v[2]), rex::byte_swap(v[3]), rex::byte_swap(v[4]),
                rex::byte_swap(v[5]));
  }
  if (data.indexAddress) {
    uint32_t indexSize = data.wideIndices ? 4 : 2;
    Upload indices = Allocate(uint64_t(data.indexCount) * indexSize, 16);
    const uint8_t* source = guest::Virtual(data.indexAddress);
    if (data.wideIndices) {
      SwapIndices<uint32_t>(indices.data, source, data.indexCount);
    } else {
      SwapIndices<uint16_t>(indices.data, source, data.indexCount);
    }
    pending.record.indexView = RenderIndexBufferView(
        RenderBufferReference(indices.buffer, indices.offset), data.indexCount * indexSize,
        data.wideIndices ? RenderFormat::R32_UINT : RenderFormat::R16_UINT);
    pending.record.indexed = true;
  }
  SubmitDraw(pending.record);
}

// Captures a draw from the device state; records it now, or keeps it for
// FlushPendingDraw when the data comes inline.
void Draw(uint32_t device, uint32_t primitive, uint32_t startVertex, uint32_t count,
          bool indexed, uint32_t startIndex, const InlineData* inlineData = nullptr) {
  auto findShader = [](uint32_t object) -> GuestShader* {
    auto it = g_guestShaders.find(object);
    return it != g_guestShaders.end() ? it->second.get() : nullptr;
  };
  GuestShader* vs = findShader(guest::Load32(device + 0x3190));
  GuestShader* ps = findShader(guest::Load32(device + 0x318C));
  RenderShader* vertexShader = vs ? GetHostShader(*vs) : nullptr;
  RenderShader* pixelShader = ps ? GetHostShader(*ps) : nullptr;
  if (!vertexShader || !pixelShader || count == 0) {
    return;
  }

  HostTexture* target = GetSurface(guest::Load32(device + guest::device::kRenderTargets), false);
  HostTexture* depth = GetSurface(guest::Load32(device + guest::device::kDepthStencil), true);
  if (!target && !depth) {
    return;
  }
  // A framebuffer's attachments may differ in size (the area is the
  // smallest); samples must match.
  if (target && depth && (depth->width < target->width || depth->height < target->height ||
                          target->samples != depth->samples)) {
    depth = nullptr;
  }

  // Shaders that write memory (bit 5 of the entry's mask, set by
  // XenosRecomp) must run even when nothing is rasterized.
  bool memExport = (vs->entry->specConstantsMask & 0x20) != 0;
  reg::PA_SU_SC_MODE_CNTL mode;
  mode.value = Register(device, 0x2205);
  bool rectList = xenos::PrimitiveType(primitive) == xenos::PrimitiveType::kRectangleList;
  if (rectList) {
    mode.cull_front = 0;  // Xenia never culls rectangle lists
    mode.cull_back = 0;
  }
  if (mode.cull_front && mode.cull_back) {
    if (!memExport) {
      return;
    }
    mode.cull_front = 0;
    mode.cull_back = 0;
  }

  // Shared constants.
  SharedConstants shared = {};
  for (auto& indices : shared.textureIndices) {
    for (auto& index : indices) {
      index = DESCRIPTOR_NULL_2D;
    }
  }
  shared.textureIndices[1][0] = DESCRIPTOR_NULL_3D;
  for (uint32_t i = 0; i < 32; i++) {
    shared.textureIndices[1][i] = DESCRIPTOR_NULL_3D;
    shared.textureIndices[2][i] = DESCRIPTOR_NULL_CUBE;
    shared.samplerIndices[i] = SAMPLER_LINEAR_CLAMP;
  }
  for (uint32_t i = 0; i < 8; i++) {
    shared.booleans[i] = guest::Load32(device + 0x2780 + i * 4);
  }
  // Textures: the 32 fetch constants that hold one (24 bytes each).
  for (uint32_t slot = 0; slot < 32; slot++) {
    if ((guest::Load32(device + 0x480 + slot * 24) & 3) !=
        uint32_t(xenos::FetchConstantType::kTexture)) {
      continue;
    }
    xenos::xe_gpu_texture_fetch_t fetch;
    uint32_t* dwords = &fetch.dword_0;
    for (uint32_t i = 0; i < 6; i++) {
      dwords[i] = guest::Load32(device + 0x480 + slot * 24 + i * 4);
    }
    uint32_t descriptor = GetTexture(fetch);
    if (Tracing()) {
      REXLOG_INFO("  tex {} fmt {} {}x{} base {:#x} tiled {} endian {} dim {} -> {}", slot,
                  uint32_t(fetch.format), fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                  uint32_t(fetch.base_address) << 12, uint32_t(fetch.tiled),
                  uint32_t(fetch.endianness), uint32_t(fetch.dimension), descriptor);
    }
    if (descriptor) {
      shared.textureIndices[0][slot] = descriptor;  // 2D
      shared.textureIndices[3][slot] = descriptor;  // 1D (sampled as Nx1 2D)
      shared.samplerIndices[slot] = GetSampler(fetch);
    }
  }
  reg::RB_COLORCONTROL colorControl;
  colorControl.value = Register(device, 0x2202);
  shared.alphaThreshold = RegisterFloat(device, 0x210E);
  shared.guestMemoryBase = g_guestMemoryAddress;
  for (uint32_t i = 0; i < 96; i++) {
    shared.vertexFetch[i][0] = guest::Load32(device + 0x480 + i * 8);
    shared.vertexFetch[i][1] = guest::Load32(device + 0x480 + i * 8 + 4);
  }
  if (inlineData) {
    // Inline data is stream 0; D3D puts its fetch constant in the command
    // stream, not in the device.
    shared.vertexFetch[95][0] = PhysicalAddress(inlineData->vertexAddress) |
                                uint32_t(xenos::FetchConstantType::kVertex);
    shared.vertexFetch[95][1] = ((inlineData->vertexBytes / 4) << 2) |
                                uint32_t(xenos::Endian::k8in32);
  }

  // Vertex data the shaders read: every valid vertex fetch constant (inline
  // data is synced when it has been written).
  for (uint32_t i = 0; i < (inlineData ? 95u : 96u); i++) {
    uint32_t word0 = shared.vertexFetch[i][0];
    uint32_t word1 = shared.vertexFetch[i][1];
    if ((word0 & 3) != uint32_t(xenos::FetchConstantType::kVertex)) {
      continue;
    }
    uint32_t address = word0 & 0xFFFFFFFC;
    uint32_t size = word1 & 0x3FFFFFC;  // size in dwords at bit 2: bytes
    if (size && size <= (16u << 20)) {
      SyncGuestMemory(address & 0x1FFFFFFF, size);
    }
  }

  // Declared vertex elements, from the vertex declaration.
  uint32_t declaration = guest::Load32(device + 0x2E24);
  if (declaration && !vs->elements.empty()) {
    uint32_t declCount = guest::Load32(declaration + 24);
    for (size_t slot = 0; slot < vs->elements.size() && slot < 16; slot++) {
      auto [usage, usageIndex] = vs->elements[slot];
      for (uint32_t e = 0; e < declCount && e < 64; e++) {
        uint32_t element = declaration + 52 + e * 12;
        uint32_t streamOffset = guest::Load32(element);
        uint32_t type = guest::Load32(element + 4);
        uint32_t usageWord = guest::Load32(element + 8);
        if (((usageWord >> 16) & 0xFF) != usage || ((usageWord >> 8) & 0xFF) != usageIndex) {
          continue;
        }
        uint32_t stream = streamOffset >> 16;
        uint32_t offset = streamOffset & 0xFFFF;
        uint32_t stride = (inlineData && stream == 0)
                              ? inlineData->stride / 4
                              : guest::Virtual(device + 0x30E8 + stream)[0];
        shared.vertexElements[slot][0] = 95 - stream;
        shared.vertexElements[slot][1] = offset / 4;
        shared.vertexElements[slot][2] = stride;
        shared.vertexElements[slot][3] = ElementFlags(type);
        break;
      }
    }
  }

  // Viewport: hardware transform, or positions in pixels.
  reg::PA_CL_VTE_CNTL vte;
  vte.value = Register(device, 0x2206);
  HostTexture* sizeSource = target ? target : depth;
  float targetWidth = float(sizeSource->width);
  float targetHeight = float(sizeSource->height);
  RenderViewport viewport(0.0f, 0.0f, targetWidth, targetHeight, 0.0f, 1.0f);
  if (vte.vport_x_scale_ena) {
    float xScale = RegisterFloat(device, 0x210F), xOffset = RegisterFloat(device, 0x2110);
    float yScale = RegisterFloat(device, 0x2111), yOffset = RegisterFloat(device, 0x2112);
    float zScale = RegisterFloat(device, 0x2113), zOffset = RegisterFloat(device, 0x2114);
    viewport = RenderViewport(xOffset - std::fabs(xScale), yOffset - std::fabs(yScale),
                              2.0f * std::fabs(xScale), 2.0f * std::fabs(yScale),
                              std::min(zOffset, zOffset + zScale),
                              std::max(zOffset, zOffset + zScale));
    // The host viewport is always top-down with positive sizes. D3D's usual
    // negative Y scale is what -fvk-invert-y (on every translated vertex
    // shader) expects; mirror only the unusual signs.
    shared.ndcTransform[0] = xScale < 0 ? -1.0f : 1.0f;
    shared.ndcTransform[1] = yScale < 0 ? 1.0f : -1.0f;
  } else {
    // Window coordinates: map pixels to NDC over the whole target.
    shared.ndcTransform[0] = 2.0f / targetWidth;
    shared.ndcTransform[1] = -2.0f / targetHeight;
    shared.ndcTransform[2] = -1.0f;
    shared.ndcTransform[3] = 1.0f;
  }

  // Float constants, little-endian.
  Upload vsConstants = Allocate(256 * 16);
  Upload psConstants = Allocate(224 * 16);
  const uint32_t* vsSource = reinterpret_cast<const uint32_t*>(guest::Virtual(device + 0x780));
  const uint32_t* psSource = reinterpret_cast<const uint32_t*>(guest::Virtual(device + 0x1780));
  for (uint32_t i = 0; i < 256 * 4; i++) {
    reinterpret_cast<uint32_t*>(vsConstants.data)[i] = rex::byte_swap(vsSource[i]);
  }
  for (uint32_t i = 0; i < 224 * 4; i++) {
    reinterpret_cast<uint32_t*>(psConstants.data)[i] = rex::byte_swap(psSource[i]);
  }
  Upload sharedUpload = Allocate(sizeof(shared));
  std::memcpy(sharedUpload.data, &shared, sizeof(shared));

  // Index data (converted to little-endian).
  uint32_t drawCount = count;
  std::optional<RenderIndexBufferView> indexView;
  auto primitiveType = xenos::PrimitiveType(primitive);
  if (inlineData && indexed) {
    // Indices arrive later (FlushPendingDraw).
  } else if (indexed) {
    uint32_t indexBuffer = guest::Load32(device + 0x308C);
    if (!indexBuffer) {
      return;
    }
    bool wide = guest::Load32(indexBuffer) & 0x80000000;
    uint32_t address = PhysicalAddress(guest::Load32(indexBuffer + 24));
    uint32_t indexSize = wide ? 4 : 2;
    Upload indices = Allocate(uint64_t(count) * indexSize, 16);
    const uint8_t* source = guest::Physical(address + startIndex * indexSize);
    if (wide) {
      SwapIndices<uint32_t>(indices.data, source, count);
    } else {
      SwapIndices<uint16_t>(indices.data, source, count);
    }
    indexView = RenderIndexBufferView(RenderBufferReference(indices.buffer, indices.offset),
                                      count * indexSize,
                                      wide ? RenderFormat::R32_UINT : RenderFormat::R16_UINT);
  } else if (primitiveType == xenos::PrimitiveType::kQuadList) {
    // Quads become two triangles each.
    uint32_t quads = count / 4;
    Upload indices = Allocate(uint64_t(quads) * 6 * 4, 16);
    auto* data = reinterpret_cast<uint32_t*>(indices.data);
    for (uint32_t q = 0; q < quads; q++) {
      uint32_t v = startVertex + q * 4;
      uint32_t* out = data + q * 6;
      out[0] = v; out[1] = v + 1; out[2] = v + 2;
      out[3] = v; out[4] = v + 2; out[5] = v + 3;
    }
    drawCount = quads * 6;
    indexView = RenderIndexBufferView(RenderBufferReference(indices.buffer, indices.offset),
                                      drawCount * 4, RenderFormat::R32_UINT);
    indexed = true;
    startVertex = 0;
  }

  PipelineKey key = {};
  key.vertexShader = vs->entry->hash;
  key.pixelShader = ps->entry->hash;
  key.primitive = primitive;
  key.colorFormat = uint32_t(target ? target->format : RenderFormat::UNKNOWN);
  key.depthFormat = uint32_t(depth ? depth->format : RenderFormat::UNKNOWN);
  key.samples = uint32_t(sizeSource->samples);
  key.blendControl = Register(device, 0x2201);
  key.colorMask = Register(device, 0x2104) & 0xF;
  key.depthControl = Register(device, 0x2200);
  key.stencilMasks = Register(device, 0x210D);
  key.cull = mode.value & 0x7;
  key.alphaTest = colorControl.alpha_test_enable;
  RenderPipeline* pipeline = GetPipeline(key, vertexShader, pixelShader);
  if (!pipeline) {
    return;
  }

  if (Tracing()) {
    for (size_t slot = 0; slot < vs->elements.size(); slot++) {
      REXLOG_INFO("  element {} usage {}/{}: fetch {} offset {} stride {} flags {:#x}", slot,
                  vs->elements[slot].first, vs->elements[slot].second,
                  shared.vertexElements[slot][0], shared.vertexElements[slot][1],
                  shared.vertexElements[slot][2], shared.vertexElements[slot][3]);
    }
    REXLOG_INFO("  fetch95 {:08x} {:08x} vs container {:#x} ps container {:#x}",
                shared.vertexFetch[95][0], shared.vertexFetch[95][1], vs->container,
                ps->container);
    REXLOG_INFO("draw prim {} count {} indexed {} vs {:016X} ps {:016X} target {}x{} fmt {} "
                "blend {:#x} depth {:#x} vte {:#x} vp {} {} {} {}",
                primitive, count, indexed, vs->entry->hash, ps->entry->hash,
                sizeSource->width, sizeSource->height, uint32_t(sizeSource->format),
                key.blendControl, key.depthControl, vte.value, viewport.x, viewport.y,
                viewport.width, viewport.height);
  }
  DrawRecord record;
  record.pipeline = pipeline;
  record.memExport = memExport;
  record.target = target;
  record.depth = depth;
  record.viewport = viewport;
  record.scissor = RenderRect(0, 0, int32_t(targetWidth), int32_t(targetHeight));
  record.pushConstants[0] = vsConstants.address;
  record.pushConstants[1] = psConstants.address;
  record.pushConstants[2] = sharedUpload.address;
  record.indexed = indexed;
  record.indexView = indexView;
  record.drawCount = drawCount;
  record.startVertex = startVertex;
  if (inlineData) {
    g_pendingDraw = PendingDraw{std::move(record), *inlineData};
  } else {
    SubmitDraw(record);
  }
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
    g_boundFramebuffer = framebuffer.get();
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
  g_frameCount++;
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
    // NG2_DEBUG_WHITE_TEXTURES=1: unbound textures read as white, to see
    // geometry before textures work.
    static const bool white = [] {
      const char* v = std::getenv("NG2_DEBUG_WHITE_TEXTURES");
      return v && v[0] == '1';
    }();
    RenderSwizzle blank = white ? RenderSwizzle::ONE : RenderSwizzle::ZERO;
    viewDesc.componentMapping = RenderComponentMapping(blank, blank, blank, blank);
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

  g_rectListShader = CreateShader(g_rect_list_gs, g_rect_list_gs_size);
  g_copyShader = CreateShader(g_copy_ps, g_copy_ps_size);

  g_guestMemory = g_device->createBuffer(RenderBufferDesc::DefaultBuffer(
      GUEST_MEMORY_SIZE, RenderBufferFlag::CONSTANT | RenderBufferFlag::STORAGE));
  g_guestMemoryAddress = g_guestMemory->getDeviceAddress();
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
  g_pipelines.clear();
  g_guestShaders.clear();
  g_hostShaders.clear();
  g_textures.clear();
  for (auto& garbage : g_garbage) {
    garbage.clear();
  }
  g_samplers.clear();
  for (auto& allocator : g_uploads) {
    allocator.chunks.clear();
  }
  g_guestMemory.reset();
  g_resolveTemps.clear();
  g_copyPipelines.clear();
  g_copyShader.reset();
  g_rectListShader.reset();
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
  FlushPendingDraw();
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
  FlushPendingDraw();
  uint32_t source = flags & 7;
  bool depth = source == 4;
  uint32_t surface = depth ? guest::Load32(device + guest::device::kDepthStencil)
                           : guest::Load32(device + guest::device::kRenderTargets + source * 4);
  HostTexture* target = GetSurface(surface, depth);
  if (Tracing()) {
    REXLOG_INFO("resolve flags {:#x} from {} to {:#x}", flags,
                target ? fmt::format("{}x{} fmt {}", target->width, target->height,
                                     uint32_t(target->format))
                       : std::string("none"),
                destTexture ? uint32_t(ReadTextureFetch(destTexture).base_address) << 12 : 0u);
  }

  if (target && destTexture && !depth) {
    auto fetch = ReadTextureFetch(destTexture);
    uint32_t address = PhysicalAddress(uint32_t(fetch.base_address) << 12);
    HostTexture* dest = GetResolveDestination(address, fetch);
    if (dest) {
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
      if (x2 > x1 && y2 > y1 && dest->format != target->format) {
        ResolveByDraw(*target, *dest, x1, y1, x2, y2, dx, dy);
      } else if (x2 > x1 && y2 > y1) {
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

void OnCreateShader(uint32_t object, uint32_t container, bool pixel) {
  std::lock_guard lock(g_mutex);
  if (!g_device || !object || !container) {
    return;
  }
  uint32_t size = guest::Load32(container + 4) + guest::Load32(container + 8);
  uint64_t hash = XXH3_64bits(guest::Virtual(container), size);
  auto shader = std::make_unique<GuestShader>();
  shader->container = container;
  shader->entry = FindShaderCacheEntry(hash);
  if (!shader->entry) {
    REXLOG_WARN("video: {} shader {:016X} at {:#x} is not in the shader cache",
                pixel ? "pixel" : "vertex", hash, container);
  }
  if (!pixel) {
    // Declared vertex elements (address, usage, usage index) in container order.
    uint32_t header = container + guest::Load32(container + 24);
    uint32_t base = guest::Load32(header + 24);
    uint32_t count = guest::Load32(header + 28);
    for (uint32_t i = 0; i < count && i < 16; i++) {
      uint32_t element = guest::Load32(header + 36 + (base + i) * 4);
      shader->elements.emplace_back(uint8_t((element >> 12) & 0xF), uint8_t((element >> 16) & 0xF));
    }
  }
  g_guestShaders[object] = std::move(shader);
}

void OnDraw(uint32_t device, uint32_t primitive, uint32_t startVertex, uint32_t count,
            bool indexed, uint32_t startIndex) {
  std::lock_guard lock(g_mutex);
  if (!g_device) {
    return;
  }
  FlushPendingDraw();
  Draw(device, primitive, startVertex, count, indexed, startIndex);
}

void OnBeginVertices(uint32_t device, uint32_t primitive, uint32_t vertexCount, uint32_t stride,
                     uint32_t vertexData) {
  std::lock_guard lock(g_mutex);
  if (!g_device || !vertexData) {
    return;
  }
  FlushPendingDraw();
  InlineData data;
  data.vertexAddress = vertexData;
  data.vertexBytes = vertexCount * stride;
  data.stride = stride;
  Draw(device, primitive, 0, vertexCount, false, 0, &data);
}

void OnBeginIndexedVertices(uint32_t device, uint32_t primitive, uint32_t baseVertex,
                            uint32_t vertexCount, uint32_t indexCount, bool wideIndices,
                            uint32_t stride, uint32_t indexData, uint32_t vertexData) {
  std::lock_guard lock(g_mutex);
  if (!g_device || !vertexData || !indexData) {
    return;
  }
  FlushPendingDraw();
  InlineData data;
  data.vertexAddress = vertexData;
  data.vertexBytes = vertexCount * stride;
  data.stride = stride;
  data.indexAddress = indexData;
  data.indexCount = indexCount;
  data.wideIndices = wideIndices;
  Draw(device, primitive, baseVertex, indexCount, true, 0, &data);
}

void OnSwap(uint32_t device, uint32_t frontBuffer) {
  (void)device;
  std::lock_guard lock(g_mutex);
  if (!g_device) {
    return;
  }
  FlushPendingDraw();
  HostTexture* front = nullptr;
  if (frontBuffer) {
    auto fetch = ReadTextureFetch(frontBuffer);
    auto it = g_resolvedTextures.find(PhysicalAddress(uint32_t(fetch.base_address) << 12));
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
