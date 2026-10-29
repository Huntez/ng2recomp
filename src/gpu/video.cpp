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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <zstd.h>

#include <plume_render_interface.h>
#include <smolv.h>

#include "guest.h"
#include "menu_label.h"
#include "shader_cache.h"

extern const uint8_t g_fullscreen_vs[];
extern const size_t g_fullscreen_vs_size;
extern const uint8_t g_present_ps[];
extern const size_t g_present_ps_size;
extern const uint8_t g_rect_list_gs[];
extern const size_t g_rect_list_gs_size;
extern const uint8_t g_copy_ps[];
extern const size_t g_copy_ps_size;
extern const uint8_t g_copy_7e3_ps[];
extern const size_t g_copy_7e3_ps_size;
extern const uint8_t g_copy_7e3_ms_ps[];
extern const size_t g_copy_7e3_ms_ps_size;
extern const uint8_t g_scale_ps[];
extern const size_t g_scale_ps_size;
extern const uint8_t g_depth_copy_ps[];
extern const size_t g_depth_copy_ps_size;
extern const uint8_t g_depth_copy_ms_ps[];
extern const size_t g_depth_copy_ms_ps_size;
extern const uint8_t g_debug_ps[];
extern const size_t g_debug_ps_size;

REXCVAR_DEFINE_INT32(render_scale, 1, "Video",
                     "Internal resolution: render targets at this many times the game's size "
                     "each way (1-4)")
    .range(1, 4);
REXCVAR_DEFINE_BOOL(vsync, true, "Video", "Wait for the display's refresh to present");
REXCVAR_DEFINE_INT32(bloom, 2, "Video", "The game's bloom: 0 off, 1 reduced (half), 2 on")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(half_pixel_offset, true, "Video",
                    "Shift drawing by half a pixel where the game uses D3D's pixel centers, "
                    "as the console does");
REXCVAR_DEFINE_INT32(anisotropic_filtering, -1, "Video",
                     "Anisotropic filtering for mipmapped textures: -1 as the game asks, "
                     "0 off, 1-5 for 1x, 2x, 4x, 8x, 16x")
    .range(-1, 5);

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
  bool float7e3 = false;  // a color target last bound as 2_10_10_10_FLOAT
  uint32_t scale = 1;     // host pixels per guest pixel (render targets, resolves)
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
std::unique_ptr<RenderShader> g_copy7e3Shader;
std::unique_ptr<RenderShader> g_copy7e3MsShader;
std::unique_ptr<RenderShader> g_scaleShader;
std::unique_ptr<RenderShader> g_debugShader;
std::unique_ptr<RenderShader> g_depthCopyShader;
std::unique_ptr<RenderShader> g_depthCopyMsShader;
// Copy pipelines by pixel shader and destination format.
std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> g_copyPipelines;
// Single-sample copies of MSAA render targets, for converting resolves.
std::unordered_map<const void*, std::unique_ptr<HostTexture>> g_resolveTemps;

// NG2_PROFILE=1: time spent per renderer stage, logged once a second.
enum class Stage { kDraw, kSync, kTexture, kPipeline, kResolve, kPresent, kCount };
constexpr const char* kStageNames[] = {"draw", "sync", "texture", "pipeline", "resolve",
                                       "present"};
struct Profile {
  bool enabled = [] {
    const char* v = std::getenv("NG2_PROFILE");
    return v && v[0] == '1';
  }();
  uint64_t nanoseconds[size_t(Stage::kCount)] = {};
  uint64_t comparedBytes = 0, uploadedBytes = 0, frames = 0;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
} g_profile;

class StageTimer {
 public:
  explicit StageTimer(Stage stage) : stage_(stage) {
    if (g_profile.enabled) {
      start_ = std::chrono::steady_clock::now();
    }
  }
  ~StageTimer() {
    if (g_profile.enabled) {
      g_profile.nanoseconds[size_t(stage_)] +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - start_)
              .count();
    }
  }

 private:
  Stage stage_;
  std::chrono::steady_clock::time_point start_;
};

void ReportProfile() {
  g_profile.frames++;
  auto now = std::chrono::steady_clock::now();
  if (!g_profile.enabled || now - g_profile.last < std::chrono::seconds(1)) {
    return;
  }
  std::string line;
  for (size_t i = 0; i < size_t(Stage::kCount); i++) {
    line += fmt::format(" {} {:.1f}", kStageNames[i], g_profile.nanoseconds[i] / 1e6);
  }
  REXLOG_INFO("video: {} frames, ms:{}; compared {:.1f} MB, uploaded {:.1f} MB", g_profile.frames,
              line, g_profile.comparedBytes / 1048576.0, g_profile.uploadedBytes / 1048576.0);
  g_profile = Profile{};
  g_profile.last = now;
}

// Guest physical memory as the GPU sees it (512 MB): vertex data read by the
// shaders and memory export targets, addressed like the console.
constexpr uint64_t GUEST_MEMORY_SIZE = 512ull << 20;
std::unique_ptr<RenderBuffer> g_guestMemory;
uint64_t g_guestMemoryAddress = 0;
// What g_guestMemory holds, byte for byte, as of the uploads recorded so far
// (calloc: untouched pages cost nothing). Ranges compare the guest's memory
// with it rather than with their own last contents: draws with different
// ranges over the same memory (a ring buffer reused with other sizes)
// overwrite each other's uploads.
uint8_t* g_guestMemoryShadow = nullptr;

// Ranges synced: the frame they were last checked in and the frame they last
// changed in. A range that hasn't changed for a frame is checked once a
// frame; one that has (a dynamic buffer the game fills in batches, which the
// null backend never stalls) is checked on every draw.
//
// Some buffers are written after the draw that reads them: the game's jobs
// fill bone matrices while the render thread goes on, since the console's
// GPU reads them only after the frame is kicked off. A range found changed
// at the end of a frame it was drawn in becomes late: its copy is still
// recorded at its first draw in a frame, but filled from guest memory just
// before the frame is submitted.
struct GuestMemoryRange {
  uint64_t checkedFrame = ~0ull;
  uint64_t changedFrame = 0;
  uint64_t lateFrame = ~0ull;  // frame its late copy was recorded in
  bool late = false;
};
std::unordered_map<uint64_t, GuestMemoryRange> g_guestMemoryRanges;
struct FrameRange {
  uint32_t address, size;
  GuestMemoryRange* range;
  uint8_t* lateCopy;  // upload memory to fill, if late
};
// Ranges drawn with this frame, each once.
std::vector<FrameRange> g_frameRanges;

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
  // Pixel shaders: color outputs written (bit n = oCn).
  uint32_t colorOutputs = 0;
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
// Render scale: render targets and the textures resolved from them have this
// many host pixels per guest pixel each way (render_scale, applied between
// frames by ApplyRenderScale).
uint32_t g_resolutionScale = 1;
// The display gamma table (UpdateGammaTable).
std::unique_ptr<HostTexture> g_gammaTable;
uint64_t g_gammaTableHash = 0;

// NG2_DEBUG_HIGHLIGHT=<vs hash>: that vertex shader's draws in magenta.
// =cycle:<seconds>: from that long after the first draw, each vertex shader
// seen in turn for 2 s (logged), to find which draws are which.
// =all: every skinned vertex shader in its own color (DebugColor, logged).
bool Highlighted(uint64_t vertexShader) {
  static const char* spec = std::getenv("NG2_DEBUG_HIGHLIGHT");
  if (!spec) {
    return false;
  }
  if (std::strcmp(spec, "all") == 0) {
    return true;
  }
  static const bool cycle = std::strncmp(spec, "cycle:", 6) == 0;
  if (!cycle) {
    static const uint64_t hash = std::strtoull(spec, nullptr, 16);
    return vertexShader == hash;
  }
  static const auto start = std::chrono::steady_clock::now();
  static const double begin = std::strtod(spec + 6, nullptr);
  double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (elapsed < begin) {
    return false;
  }
  static std::vector<uint64_t> seen;
  if (std::find(seen.begin(), seen.end(), vertexShader) == seen.end()) {
    seen.push_back(vertexShader);
  }
  size_t index = size_t((elapsed - begin) / 2.0) % seen.size();
  static size_t logged = ~size_t(0);
  if (index != logged) {
    logged = index;
    REXLOG_INFO("video: highlighting vs {:016X} ({} of {})", seen[index], index, seen.size());
  }
  return vertexShader == seen[index];
}

// The highlight color for a vertex shader: magenta, or with
// NG2_DEBUG_HIGHLIGHT=all one from its hash, logged the first time.
void DebugColor(uint64_t vertexShader, float color[4]) {
  static const char* spec = std::getenv("NG2_DEBUG_HIGHLIGHT");
  if (!spec || std::strcmp(spec, "all") != 0) {
    color[0] = 1.0f, color[1] = 0.0f, color[2] = 1.0f, color[3] = 1.0f;
    return;
  }
  uint64_t mixed = XXH3_64bits(&vertexShader, sizeof(vertexShader));
  uint32_t rgb[3] = {uint32_t(mixed & 0xFF), uint32_t((mixed >> 8) & 0xFF),
                     uint32_t((mixed >> 16) & 0xFF)};
  static std::unordered_set<uint64_t> logged;
  if (logged.insert(vertexShader).second) {
    REXLOG_INFO("video: vs {:016X} color {} {} {}", vertexShader, rgb[0], rgb[1], rgb[2]);
  }
  for (int i = 0; i < 3; i++) {
    color[i] = float(rgb[i]) / 255.0f;
  }
  color[3] = 1.0f;
}

// NG2_TRACE_FRAME=<n>: log what frame n draws; t<seconds>: the first frame
// that starts that long after the first draw. A +<count> suffix traces that
// many frames from there.
bool Tracing() {
  static const char* spec = std::getenv("NG2_TRACE_FRAME");
  static const bool byTime = spec && spec[0] == 't';
  static int64_t frame = spec && !byTime ? std::strtoll(spec, nullptr, 10) : -1ll;
  static const int64_t count = [] {
    const char* plus = spec ? std::strchr(spec, '+') : nullptr;
    return plus ? std::max<int64_t>(std::strtoll(plus + 1, nullptr, 10), 1) : 1;
  }();
  if (byTime && frame < 0) {
    static const auto start = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - start >=
        std::chrono::duration<double>(std::strtod(spec + 1, nullptr))) {
      frame = int64_t(g_frameCount) + 1;
    }
  }
  return frame >= 0 && int64_t(g_frameCount) >= frame && int64_t(g_frameCount) < frame + count;
}

std::unordered_map<uint64_t, std::pair<uint32_t, std::unique_ptr<RenderSampler>>> g_samplers;
uint32_t g_nextSampler = SAMPLER_FIRST_FREE;

// Render targets by EDRAM placement and shape.
struct SurfaceKey {
  uint32_t edramBase;
  uint32_t format;  // host RenderFormat, or 0x100 for depth
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

// Textures written by resolves, by guest physical base address and shape
// (ResolvedKey): different shapes can share an address, e.g. two shadow map
// sizes resolved to the same memory in one frame.
std::unordered_map<uint64_t, std::unique_ptr<HostTexture>> g_resolvedTextures;
// The latest resolve to each address, for a texture that views it with
// another shape or format.
std::unordered_map<uint32_t, HostTexture*> g_lastResolve;
// Resolves don't write guest memory, so the memory a resolve targets keeps
// its old bytes. When the CPU later writes there (a texture loaded into memory
// that was once a render target), the resolved textures for that address are
// stale and the guest texture is read instead.
struct ResolvedMemory {
  uint32_t size = 0;
  uint64_t hash = 0;
  uint64_t checkedFrame = ~0ull;
  bool stale = false;
};
std::unordered_map<uint32_t, ResolvedMemory> g_resolvedMemory;

// Framebuffers by attachment.
struct FramebufferKey {
  const HostTexture* colors[4] = {};
  const HostTexture* depth = nullptr;
  bool operator==(const FramebufferKey&) const = default;
};
struct FramebufferKeyHash {
  size_t operator()(const FramebufferKey& k) const {
    return XXH3_64bits(&k, sizeof(k));
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
    const auto& key = it->first;
    if (std::find(std::begin(key.colors), std::end(key.colors), texture) != std::end(key.colors) ||
        key.depth == texture) {
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
    case xenos::TextureFormat::k_16_16_16_16_EXPAND:
      return RenderFormat::R16G16B16A16_FLOAT;
    case xenos::TextureFormat::k_16_16:
      return RenderFormat::R16G16_SNORM;
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_EXPAND:
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
  // By host format: guest formats that share a host one (2_10_10_10_FLOAT and
  // its AS_16_16_16_16 view, the two depth formats) are the same EDRAM data
  // drawn and resolved through either.
  uint32_t guestFormat = (info >> 16) & (depth ? 1 : 0xF);
  key.format = depth ? 0x100 : uint32_t(ColorTargetFormat(guestFormat));
  key.width = ((dims >> 18) & 0x3FFF) + 1;
  key.height = ((dims >> 3) & 0x7FFF) + 1;
  key.msaa = guest::Load16(surface + 24) & 3;

  auto& slot = g_surfaces[key];
  if (!slot) {
    auto host = std::make_unique<HostTexture>();
    host->scale = g_resolutionScale;
    host->width = key.width * host->scale;
    host->height = key.height * host->scale;
    host->samples = SampleCount(key.msaa);
    host->format = depth ? RenderFormat::D32_FLOAT_S8_UINT : ColorTargetFormat(guestFormat);
    RenderTextureDesc desc = RenderTextureDesc::Texture2D(
        host->width, host->height, 1, host->format,
        depth ? RenderTextureFlag::DEPTH_TARGET : RenderTextureFlag::RENDER_TARGET);
    desc.multisampling.sampleCount = host->samples;
    host->texture = g_device->createTexture(desc);
    if (!host->texture) {
      // Out of video memory, most likely at a high render scale.
      REXLOG_ERROR("video: no {}x{} render target", host->width, host->height);
      return nullptr;
    }
    REXLOG_INFO("video: {} target {}x{} edram {:#x} format {} msaa {} (scale {})",
                depth ? "depth" : "color", key.width, key.height, key.edramBase, guestFormat,
                key.msaa, host->scale);
    slot = std::move(host);
  }
  // 2_10_10_10_FLOAT and its 16_16_16_16 view hold 7e3 color (ResolveByDraw).
  slot->float7e3 = !depth && (guestFormat == 3 || guestFormat == 12);
  return slot.get();
}

// Binds colors[0..colorCount) (no gaps) and depth.
void BindFramebuffer(HostTexture* const* colors, uint32_t colorCount, HostTexture* depth) {
  FramebufferKey key;
  for (uint32_t i = 0; i < colorCount; i++) {
    key.colors[i] = colors[i];
  }
  key.depth = depth;
  auto& framebuffer = g_framebuffers[key];
  if (!framebuffer) {
    const RenderTexture* attachments[4] = {};
    for (uint32_t i = 0; i < colorCount; i++) {
      attachments[i] = colors[i]->texture.get();
    }
    RenderFramebufferDesc desc;
    desc.colorAttachments = colorCount ? attachments : nullptr;
    desc.colorAttachmentsCount = colorCount;
    desc.depthAttachment = depth ? depth->texture.get() : nullptr;
    framebuffer = g_device->createFramebuffer(desc);
  }
  for (uint32_t i = 0; i < colorCount; i++) {
    Transition(*colors[i], RenderTextureLayout::COLOR_WRITE);
  }
  if (depth) {
    Transition(*depth, RenderTextureLayout::DEPTH_WRITE);
  }
  if (g_boundFramebuffer != framebuffer.get()) {
    CommandList()->setFramebuffer(framebuffer.get());
    g_boundFramebuffer = framebuffer.get();
  }
}

void BindFramebuffer(HostTexture* color, HostTexture* depth) {
  BindFramebuffer(&color, color ? 1 : 0, depth);
}

uint64_t ResolvedKey(uint32_t address, const xenos::xe_gpu_texture_fetch_t& fetch) {
  uint32_t data[4] = {address, uint32_t(fetch.size_2d.width), uint32_t(fetch.size_2d.height),
                      uint32_t(ResolveTextureFormat(fetch.format))};
  return XXH3_64bits(data, sizeof(data));
}

// Host texture backing a guest texture written by resolves.
// scale: the source's, so the copy is pixel for pixel.
HostTexture* GetResolveDestination(uint32_t address, const xenos::xe_gpu_texture_fetch_t& fetch,
                                   uint32_t scale) {
  RenderFormat format = ResolveTextureFormat(fetch.format);
  if (format == RenderFormat::UNKNOWN) {
    return nullptr;
  }
  uint32_t width = fetch.size_2d.width + 1;
  uint32_t height = fetch.size_2d.height + 1;
  auto& slot = g_resolvedTextures[ResolvedKey(address, fetch)];
  if (slot && slot->scale != scale) {
    g_garbage[g_frame].push_back(std::move(slot));  // resolved at another render scale
  }
  if (!slot) {
    auto host = std::make_unique<HostTexture>();
    host->scale = scale;
    host->width = width * scale;
    host->height = height * scale;
    host->format = format;
    host->texture = g_device->createTexture(RenderTextureDesc::Texture2D(
        host->width, host->height, 1, format, RenderTextureFlag::RENDER_TARGET));
    if (!host->texture) {
      REXLOG_ERROR("video: no {}x{} resolve texture", host->width, host->height);
      return nullptr;
    }
    // Depth textures read their single channel everywhere, as Xenos does.
    if (fetch.format == xenos::TextureFormat::k_24_8 ||
        fetch.format == xenos::TextureFormat::k_24_8_FLOAT) {
      RenderTextureViewDesc viewDesc = RenderTextureViewDesc::Texture2D(format);
      viewDesc.mipLevels = 1;
      viewDesc.componentMapping = RenderComponentMapping(RenderSwizzle::R, RenderSwizzle::R,
                                                         RenderSwizzle::R, RenderSwizzle::R);
      host->view = host->texture->createTextureView(viewDesc);
    }
    SetDescriptor(*host);
    REXLOG_INFO("video: resolve texture {:#x} {}x{} format {}", address, width, height,
                uint32_t(fetch.format));
    slot = std::move(host);
  }
  g_lastResolve[address] = slot.get();
  auto& memory = g_resolvedMemory[address];
  memory.size = uint32_t(std::min<uint64_t>(uint64_t(width) * height * RenderFormatSize(format),
                                            GUEST_MEMORY_SIZE - address));
  memory.hash = XXH3_64bits(guest::Physical(address), memory.size);
  memory.checkedFrame = g_frameCount;
  memory.stale = false;
  return slot.get();
}

// False once the CPU has overwritten the memory resolved to address.
bool ResolveCurrent(uint32_t address) {
  auto it = g_resolvedMemory.find(address);
  if (it == g_resolvedMemory.end()) {
    return true;
  }
  auto& memory = it->second;
  if (!memory.stale && memory.checkedFrame != g_frameCount) {
    memory.checkedFrame = g_frameCount;
    memory.stale = XXH3_64bits(guest::Physical(address), memory.size) != memory.hash;
    if (memory.stale && Tracing()) {
      REXLOG_INFO("video: resolve memory {:#x} overwritten", address);
    }
  }
  return !memory.stale;
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
// Copies a rect of a render target (color, or depth into R32_FLOAT) to a
// texture by drawing, converting formats.
void ResolveByDraw(HostTexture& source, HostTexture& dest, int32_t x1, int32_t y1, int32_t x2,
                   int32_t y2, uint32_t dx, uint32_t dy, bool depth = false) {
  HostTexture* readable = &source;
  bool multisampled = source.samples != RenderSampleCount::COUNT_1;
  RenderShader* shader = g_copyShader.get();
  if (depth) {
    shader = multisampled ? g_depthCopyMsShader.get() : g_depthCopyShader.get();
  } else if (source.float7e3) {
    // Each sample is rounded before averaging, so no hardware resolve.
    shader = multisampled ? g_copy7e3MsShader.get() : g_copy7e3Shader.get();
  } else if (multisampled) {
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

  auto& pipeline =
      g_copyPipelines[(uint64_t(reinterpret_cast<uintptr_t>(shader)) << 8) ^ uint64_t(dest.format)];
  if (!pipeline) {
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = g_pipelineLayout.get();
    desc.vertexShader = g_fullscreenShader.get();
    desc.pixelShader = shader;
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

void RecordGuestMemoryCopy(uint32_t address, const Upload& upload, uint32_t size) {
  auto* commandList = CommandList();
  commandList->barriers(RenderBarrierStage::COPY,
                        RenderBufferBarrier(g_guestMemory.get(), RenderBufferAccess::WRITE));
  commandList->copyBufferRegion(RenderBufferReference(g_guestMemory.get(), address),
                                RenderBufferReference(upload.buffer, upload.offset), size);
  commandList->barriers(RenderBarrierStage::GRAPHICS,
                        RenderBufferBarrier(g_guestMemory.get(),
                                            RenderBufferAccess::READ | RenderBufferAccess::WRITE));
}

// Makes guest physical [address, address + size) current in g_guestMemory.
// always: inline vertex data in the ring buffer, rewritten at any time.
void SyncGuestMemory(uint32_t address, uint32_t size, bool always = false) {
  if (!size || uint64_t(address) + size > GUEST_MEMORY_SIZE) {
    return;
  }
  StageTimer timer(Stage::kSync);
  auto& range = g_guestMemoryRanges[(uint64_t(address) << 32) | size];
  if (range.late) {
    if (range.lateFrame != g_frameCount) {
      range.lateFrame = g_frameCount;
      Upload upload = Allocate(size, 16);
      RecordGuestMemoryCopy(address, upload, size);
      g_frameRanges.push_back({address, size, &range, upload.data});
    }
    return;
  }
  bool first = range.checkedFrame == ~0ull;
  if (!always && range.checkedFrame != g_frameCount) {
    g_frameRanges.push_back({address, size, &range, nullptr});
  }
  bool stable = !first && range.changedFrame + 1 < g_frameCount;
  if (!always && stable && range.checkedFrame == g_frameCount) {
    return;
  }
  range.checkedFrame = g_frameCount;
  const uint8_t* source = guest::Physical(address);
  uint8_t* shadow = g_guestMemoryShadow + address;
  g_profile.comparedBytes += size;
  // Upload only the span between the first and last block that differ.
  uint32_t begin = 0, end = size;
  if (!first) {
    if (std::memcmp(source, shadow, size) == 0) {
      return;
    }
    constexpr uint32_t kBlock = 256;
    while (begin < size &&
           std::memcmp(source + begin, shadow + begin, std::min(kBlock, size - begin)) == 0) {
      begin += kBlock;
    }
    while (end > begin) {
      uint32_t blockStart = std::max(begin, (end - 1) / kBlock * kBlock);
      if (std::memcmp(source + blockStart, shadow + blockStart, end - blockStart) != 0) {
        break;
      }
      end = blockStart;
    }
  }
  uint32_t length = end - begin;
  range.changedFrame = g_frameCount;
  g_profile.uploadedBytes += length;
  std::memcpy(shadow + begin, source + begin, length);
  Upload upload = Allocate(length, 16);
  std::memcpy(upload.data, source + begin, length);
  RecordGuestMemoryCopy(address + begin, upload, length);
}

// End of frame, before submitting: fills the late ranges' copies and finds
// the ranges written after their draws.
void FinishGuestMemory() {
  // Find the ranges written after their draws first, against the shadow
  // before the late copies below change it.
  for (auto& frameRange : g_frameRanges) {
    auto& range = *frameRange.range;
    if (frameRange.lateCopy ||
        std::memcmp(guest::Physical(frameRange.address),
                    g_guestMemoryShadow + frameRange.address, frameRange.size) == 0) {
      continue;
    }
    if (range.changedFrame + 1 >= g_frameCount) {
      // Checked at every draw this frame and still written after the last.
      range.late = true;
      REXLOG_DEBUG("video: {:#x} ({} bytes) is written after its draws", frameRange.address,
                   frameRange.size);
    } else {
      // Checked once this frame: next frame, check it at every draw first
      // (it may only be filled between two of its draws).
      range.changedFrame = g_frameCount;
    }
  }
  for (auto& frameRange : g_frameRanges) {
    if (frameRange.lateCopy) {
      const uint8_t* source = guest::Physical(frameRange.address);
      std::memcpy(frameRange.lateCopy, source, frameRange.size);
      std::memcpy(g_guestMemoryShadow + frameRange.address, source, frameRange.size);
      g_profile.uploadedBytes += frameRange.size;
    }
  }
  g_frameRanges.clear();
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
    case F::k_16_16_16_16_FLOAT:
    case F::k_16_16_16_16_EXPAND: return {RenderFormat::R16G16B16A16_FLOAT, 1, 8};
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
  // The texture views hold only the levels the fetch constant has.
  // Without a base address only the mips are there (Xenia's
  // GetSubresourcesFromFetchConstant).
  desc.minLOD = float(fetch.base_address ? uint32_t(fetch.mip_min_level)
                                         : std::max(uint32_t(fetch.mip_min_level), 1u));
  desc.maxLOD = fetch.mip_filter == xenos::TextureFilter::kBaseMap ? desc.minLOD : FLT_MAX;
  desc.mipLODBias = std::clamp(float(fetch.lod_bias) / 32.0f, -15.0f, 15.0f);
  // Anisotropy as the fetch constant asks (1x-16x), for filtered mipmapped
  // sampling like Xenia's; it implies linear filtering.
  uint32_t aniso = uint32_t(fetch.aniso_filter);
  if (int32_t forced = REXCVAR_GET(anisotropic_filtering); forced >= 0) {
    aniso = uint32_t(forced);  // the player's choice, as Xenia's anisotropic_override
  }
  if (aniso >= 2 && aniso <= 5 && fetch.mip_filter != xenos::TextureFilter::kBaseMap &&
      desc.minFilter == RenderFilter::LINEAR && desc.magFilter == RenderFilter::LINEAR) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = 1u << (aniso - 1);
  }
  // By field: the desc has padding bytes.
  float lod[3] = {desc.minLOD, desc.maxLOD, desc.mipLODBias};
  uint32_t keyData[10] = {uint32_t(desc.addressU), uint32_t(desc.addressV),
                          uint32_t(desc.addressW), uint32_t(desc.magFilter),
                          uint32_t(desc.minFilter), uint32_t(desc.mipmapMode),
                          desc.anisotropyEnabled ? desc.maxAnisotropy : 0u};
  std::memcpy(&keyData[7], lod, sizeof(lod));
  uint64_t hash = XXH3_64bits(keyData, sizeof(keyData));
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

// Where each level of a guest texture lies in memory (Xenia's
// texture_util::GetGuestTextureLayout and GetPackedMipOffset, 2D only).
struct LevelLayout {
  uint32_t width, height;     // texels
  uint32_t pitchBlocks;       // row pitch, aligned to a 32-block tile
  uint32_t offsetX, offsetY;  // blocks into the packed mip tail
  uint64_t start;             // bytes from the base or mip address
  uint64_t size;              // bytes from start the level spans
};
struct TextureLayout {
  std::vector<LevelLayout> levels;  // the base, then the mips
  uint32_t mipAddress = 0;          // guest physical
  uint64_t mipsSize = 0;            // bytes from mipAddress the mips span
};

uint32_t Log2Ceil(uint32_t value) { return uint32_t(std::bit_width(value - 1)); }

// Bytes a level spans from its start. Tiled addresses of the last tile can
// reach past pitch x rows (the 8-bit last row of a 300x145 HUD texture):
// Xenia's GetTiledAddressUpperBound2D, from the origin of the last 32x32 tile.
uint64_t LevelExtent(bool tiled, uint32_t pitchBlocks, uint32_t rightBlocks,
                     uint32_t bottomBlocks, uint32_t bytesPerBlock) {
  if (!tiled) {
    return uint64_t(pitchBlocks) * (bottomBlocks - 1) * bytesPerBlock +
           uint64_t(rightBlocks) * bytesPerBlock;
  }
  uint32_t bppLog2 = uint32_t(std::countr_zero(bytesPerBlock));
  uint64_t bound = uint32_t(TiledOffset2D(int32_t((rightBlocks - 1) & ~31u),
                                          int32_t((bottomBlocks - 1) & ~31u), pitchBlocks,
                                          bppLog2));
  return bound + (bppLog2 == 0 ? 0xA00 : bppLog2 == 1 ? 0xC00 : 0x400u << bppLog2);
}

// Levels from packedLevel on share one 32x32 tile, each at its own offset:
// 16 texels across for the first (down for wide textures), then 8 and 4, then
// the 2x2 and 1x1 ones along the other axis.
void PackedMipOffset(uint32_t width, uint32_t height, uint32_t level, uint32_t blockSize,
                     uint32_t& x, uint32_t& y) {
  uint32_t log2Width = Log2Ceil(width), log2Height = Log2Ceil(height);
  uint32_t log2Size = std::min(log2Width, log2Height);
  uint32_t packedBase = log2Size > 4 ? log2Size - 4 : 0;
  uint32_t packed = level - packedBase;
  bool wide = log2Width > std::max(log2Height, 1u);  // 2x1 packs like a square
  x = y = 0;
  if (packed < 3) {
    (wide ? y : x) = 16 >> packed;
  } else if (wide) {
    x = (1u << (log2Width - packedBase)) >> (packed - 2);
  } else {
    y = (1u << (log2Height - packedBase)) >> (packed - 2);
  }
  x /= blockSize;
  y /= blockSize;
}

TextureLayout GetTextureLayout(const xenos::xe_gpu_texture_fetch_t& fetch,
                               const TextureFormatInfo& info, uint32_t width, uint32_t height) {
  TextureLayout layout;
  auto blocks = [&](uint32_t texels) { return (texels + info.blockSize - 1) / info.blockSize; };
  bool oneD = fetch.dimension == xenos::DataDimension::k1D;
  // (1D textures don't pack their mips: Xenia ignores the flag there.)
  uint32_t log2Size = std::min(Log2Ceil(width), Log2Ceil(height));
  uint32_t packedLevel =
      fetch.packed_mips && !oneD ? (log2Size > 4 ? log2Size - 4 : 0) : ~0u;

  LevelLayout base = {width, height};
  uint32_t pitchTexels = std::max(uint32_t(fetch.pitch) << 5, width);
  base.pitchBlocks = (blocks(pitchTexels) + 31) & ~31u;
  if (packedLevel == 0) {
    PackedMipOffset(width, height, 0, info.blockSize, base.offsetX, base.offsetY);
  }
  base.size = LevelExtent(fetch.tiled, base.pitchBlocks, blocks(width) + base.offsetX,
                          blocks(height) + base.offsetY, info.bytesPerBlock);
  layout.levels.push_back(base);

  // With the base in the mip tail, the whole tail is at the base address.
  uint32_t mipPage = packedLevel == 0 && fetch.base_address ? uint32_t(fetch.base_address)
                                                            : uint32_t(fetch.mip_address);
  // No mips for 1D or stacked textures (sampled as one 2D slice).
  uint32_t sizeMaxLevel = uint32_t(std::bit_width(std::max(width, height)) - 1);
  uint32_t maxLevel = 0;
  if (mipPage && !oneD && !fetch.stacked) {
    maxLevel = std::max(std::min(uint32_t(fetch.mip_max_level), sizeMaxLevel),
                        std::min(uint32_t(fetch.mip_min_level), sizeMaxLevel));
  }
  layout.mipAddress = mipPage << 12;
  // Mips are laid out as for power-of-two base sizes, each level's rows
  // aligned to 32 blocks (linear ones to 256 bytes) and its storage to 4 KB.
  uint32_t pitchAlignment = fetch.tiled ? 32 : std::max(256 / info.bytesPerBlock, 32u);
  uint64_t offset = 0;
  for (uint32_t level = 1; level <= maxLevel; level++) {
    uint32_t stored = std::min(level, packedLevel);
    LevelLayout mip = {std::max(width >> level, 1u), std::max(height >> level, 1u)};
    uint32_t pitch = blocks(std::max(std::bit_ceil(width) >> stored, 1u));
    mip.pitchBlocks = (pitch + pitchAlignment - 1) / pitchAlignment * pitchAlignment;
    if (level >= packedLevel) {
      PackedMipOffset(width, height, level, info.blockSize, mip.offsetX, mip.offsetY);
    }
    mip.start = offset;
    mip.size = LevelExtent(fetch.tiled, mip.pitchBlocks, blocks(mip.width) + mip.offsetX,
                           blocks(mip.height) + mip.offsetY, info.bytesPerBlock);
    layout.mipsSize = std::max(layout.mipsSize, mip.start + mip.size);
    if (level < packedLevel) {
      uint32_t rows = (blocks(std::max(std::bit_ceil(height) >> stored, 1u)) + 31) & ~31u;
      offset += (uint64_t(mip.pitchBlocks) * rows * info.bytesPerBlock + 0xFFF) & ~0xFFFull;
    }
    layout.levels.push_back(mip);
  }
  return layout;
}

// Untiles one level into rows of its blocks. data and size: the memory at
// the level's base or mip address, endian swapped.
void CopyLevel(const xenos::xe_gpu_texture_fetch_t& fetch, const TextureFormatInfo& info,
               const LevelLayout& level, const uint8_t* data, uint64_t size, uint8_t* out) {
  uint32_t blocksX = (level.width + info.blockSize - 1) / info.blockSize;
  uint32_t blocksY = (level.height + info.blockSize - 1) / info.blockSize;
  uint32_t bppLog2 = uint32_t(std::countr_zero(info.bytesPerBlock));
  uint64_t rowBytes = uint64_t(blocksX) * info.bytesPerBlock;
  uint64_t linearRowBytes = uint64_t(level.pitchBlocks) * info.bytesPerBlock;
  if (level.start >= size) {
    return;
  }
  data += level.start;
  size -= level.start;
  for (uint32_t y = 0; y < blocksY; y++) {
    uint8_t* row = out + y * rowBytes;
    uint32_t guestY = y + level.offsetY;
    if (fetch.tiled) {
      for (uint32_t x = 0; x < blocksX; x++) {
        int32_t offset = TiledOffset2D(int32_t(x + level.offsetX), int32_t(guestY),
                                       level.pitchBlocks, bppLog2);
        if (offset >= 0 && uint64_t(offset) + info.bytesPerBlock <= size) {
          std::memcpy(row + x * info.bytesPerBlock, data + offset, info.bytesPerBlock);
        }
      }
    } else {
      uint64_t start = guestY * linearRowBytes + uint64_t(level.offsetX) * info.bytesPerBlock;
      if (start + rowBytes <= size) {
        std::memcpy(row, data + start, rowBytes);
      }
    }
  }
}

// Uploads a guest texture and its mips: endian swap, untile.
std::unique_ptr<HostTexture> CreateTexture(const xenos::xe_gpu_texture_fetch_t& fetch,
                                           const TextureFormatInfo& info,
                                           const TextureLayout& layout, const uint8_t* baseData,
                                           uint64_t baseSize, const uint8_t* mipData,
                                           uint64_t mipSize, bool* retry = nullptr) {
  uint32_t width = layout.levels[0].width, height = layout.levels[0].height;
  std::vector<uint8_t> base(baseData, baseData + baseSize);
  EndianSwap(base.data(), base.size(), fetch.endianness);
  std::vector<uint8_t> mips(mipData, mipData + mipSize);
  EndianSwap(mips.data(), mips.size(), fetch.endianness);

  std::vector<Upload> uploads;
  uint32_t labelRows = 0;  // rows of the base level a menu label rewrote
  for (size_t i = 0; i < layout.levels.size(); i++) {
    const LevelLayout& level = layout.levels[i];
    uint64_t bytes = uint64_t((level.width + info.blockSize - 1) / info.blockSize) *
                     ((level.height + info.blockSize - 1) / info.blockSize) * info.bytesPerBlock;
    Upload upload = Allocate(bytes, 16);
    std::memset(upload.data, 0, bytes);
    if (i == 0) {
      CopyLevel(fetch, info, level, base.data(), base.size(), upload.data);
      if (info.format == RenderFormat::R8_UNORM &&
          RewriteMenuLabel(level.width, level.height, upload.data, &labelRows) ==
              LabelResult::kRetry &&
          retry) {
        *retry = true;
      }
    } else {
      CopyLevel(fetch, info, level, mips.data(), mips.size(), upload.data);
      // The rewritten rows' mips: 2x2 averages of the level above.
      const LevelLayout& above = layout.levels[i - 1];
      uint32_t rows = std::min((labelRows + (1u << i) - 1) >> i, level.height);
      for (uint32_t y = 0; labelRows && y < rows; y++) {
        for (uint32_t x = 0; x < level.width; x++) {
          uint32_t sum = 0;
          for (uint32_t k = 0; k < 4; k++) {
            uint32_t sx = std::min(2 * x + (k & 1), above.width - 1);
            uint32_t sy = std::min(2 * y + (k >> 1), above.height - 1);
            sum += uploads[i - 1].data[sy * above.width + sx];
          }
          upload.data[y * level.width + x] = uint8_t((sum + 2) / 4);
        }
      }
    }
    uploads.push_back(upload);
  }
  // The base level, for the dumps below.
  uint32_t blocksX = (width + info.blockSize - 1) / info.blockSize;
  uint32_t blocksY = (height + info.blockSize - 1) / info.blockSize;
  uint64_t rowBytes = uint64_t(blocksX) * info.bytesPerBlock;
  const Upload& upload = uploads[0];

  // NG2_DUMP_TEXTURES=<dir>: write decoded 8-bit and 8888 textures (PGM/PAM).
  static const char* dumpDir = std::getenv("NG2_DUMP_TEXTURES");
  // Block-compressed textures as DDS (DXT1/DXT3/DXT5).
  if (dumpDir && (info.format == RenderFormat::BC1_UNORM ||
                  info.format == RenderFormat::BC2_UNORM ||
                  info.format == RenderFormat::BC3_UNORM)) {
    std::string path = fmt::format("{}/{:08x}_{}x{}.dds", dumpDir,
                                   uint32_t(fetch.base_address) << 12, width, height);
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      uint32_t header[32] = {};
      header[0] = 0x20534444;  // "DDS "
      header[1] = 124;
      header[2] = 0x1007;  // caps, height, width, pixel format
      header[3] = height;
      header[4] = width;
      header[19] = 32;
      header[20] = 4;  // FOURCC
      header[21] = info.format == RenderFormat::BC1_UNORM   ? 0x31545844   // DXT1
                   : info.format == RenderFormat::BC2_UNORM ? 0x33545844   // DXT3
                                                            : 0x35545844;  // DXT5
      header[27] = 0x1000;
      std::fwrite(header, 4, 32, f);
      std::fwrite(upload.data, 1, rowBytes * blocksY, f);
      std::fclose(f);
    }
  }
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
  uint32_t levelCount = uint32_t(layout.levels.size());
  host->texture = g_device->createTexture(
      RenderTextureDesc::Texture2D(width, height, levelCount, info.format));
  Transition(*host, RenderTextureLayout::COPY_DEST);
  for (uint32_t i = 0; i < levelCount; i++) {
    const LevelLayout& level = layout.levels[i];
    uint32_t rowTexels = (level.width + info.blockSize - 1) / info.blockSize * info.blockSize;
    CommandList()->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(host->texture.get(), i),
        RenderTextureCopyLocation::PlacedFootprint(uploads[i].buffer, info.format, level.width,
                                                   level.height, 1, rowTexels,
                                                   uploads[i].offset));
  }

  RenderTextureViewDesc viewDesc;
  viewDesc.format = info.format;
  viewDesc.dimension = RenderTextureViewDimension::TEXTURE_2D;
  viewDesc.mipLevels = levelCount;
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

// Logs each kind of texture the renderer can't sample yet, once.
void ReportUnsupportedTexture(const char* reason, const xenos::xe_gpu_texture_fetch_t& fetch) {
  static std::unordered_set<uint32_t> reported;
  uint32_t kind = (uint32_t(fetch.format) << 8) | (uint32_t(fetch.dimension) << 4) |
                  (reason[0] & 0xF);
  if (reported.insert(kind).second) {
    REXLOG_WARN("video: texture skipped ({}): format {} dimension {}", reason,
                uint32_t(fetch.format), uint32_t(fetch.dimension));
  }
}

// Descriptor index for a texture fetch constant, 0 if it can't be sampled.
// The bloom (blurred bright parts, blended with the previous frame's) is
// added to the picture by two passes, both from fetch slot 1: tone mapping
// and the last full-screen composite. The bloom setting acts on what those
// two read; the pass that blends in the previous frame still reads it all.
constexpr uint64_t kToneMapPixelShader = 0x2CB124864F1A4460;
constexpr uint64_t kCompositePixelShader = 0xEAE805C57979C8A5;
constexpr uint32_t kBloomSlot = 1;
std::unique_ptr<HostTexture> g_reducedBloom;  // bloom 1: the bloom at half strength

// The bloom as the composite should read it (descriptor with the scale bits),
// for bloom settings below 2.
uint32_t AdjustBloom(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t descriptor) {
  int32_t setting = REXCVAR_GET(bloom);
  if (setting >= 2 || !descriptor) {
    return descriptor;
  }
  if (setting <= 0) {
    return DESCRIPTOR_NULL_2D;  // blank: nothing added
  }
  // The resolved texture behind the descriptor (GetTexture's lookup).
  uint32_t base = uint32_t(fetch.base_address) << 12;
  HostTexture* source = nullptr;
  if (!ResolveCurrent(base)) {
    // GetTexture read guest memory instead
  } else if (auto it = g_resolvedTextures.find(ResolvedKey(base, fetch));
             it != g_resolvedTextures.end()) {
    source = it->second.get();
  } else if (auto last = g_lastResolve.find(base); last != g_lastResolve.end()) {
    source = last->second;
  }
  if (!source || !source->descriptor) {
    return descriptor;
  }
  auto& dest = g_reducedBloom;
  if (dest && (dest->width != source->width || dest->height != source->height ||
               dest->format != source->format || dest->scale != source->scale)) {
    g_garbage[g_frame].push_back(std::move(dest));
  }
  if (!dest) {
    dest = std::make_unique<HostTexture>();
    dest->width = source->width;
    dest->height = source->height;
    dest->format = source->format;
    dest->scale = source->scale;
    dest->texture = g_device->createTexture(RenderTextureDesc::Texture2D(
        dest->width, dest->height, 1, dest->format, RenderTextureFlag::RENDER_TARGET));
    if (!dest->texture) {
      dest.reset();
      return descriptor;
    }
    SetDescriptor(*dest);
  }
  auto& pipeline = g_copyPipelines[(uint64_t(reinterpret_cast<uintptr_t>(g_scaleShader.get())) << 8) ^
                                   uint64_t(dest->format)];
  if (!pipeline) {
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = g_pipelineLayout.get();
    desc.vertexShader = g_fullscreenShader.get();
    desc.pixelShader = g_scaleShader.get();
    desc.renderTargetFormat[0] = dest->format;
    desc.renderTargetBlend[0] = RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    pipeline = g_device->createGraphicsPipeline(desc);
  }
  Transition(*source, RenderTextureLayout::SHADER_READ);
  BindFramebuffer(dest.get(), nullptr);
  auto* commandList = CommandList();
  RenderViewport viewport(0.0f, 0.0f, float(dest->width), float(dest->height));
  RenderRect scissor(0, 0, int32_t(dest->width), int32_t(dest->height));
  float factor = 0.5f;
  uint32_t constants[2] = {source->descriptor, 0};
  std::memcpy(&constants[1], &factor, sizeof(factor));
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
  Transition(*dest, RenderTextureLayout::SHADER_READ);
  return dest->descriptor | ((dest->scale - 1) << 20);
}

uint32_t GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  StageTimer timer(Stage::kTexture);
  bool oneD = fetch.dimension == xenos::DataDimension::k1D;
  if (!oneD && fetch.dimension != xenos::DataDimension::k2DOrStacked) {
    ReportUnsupportedTexture("3D or cube", fetch);
    return 0;
  }
  uint32_t base = uint32_t(fetch.base_address) << 12;

  // Render-to-texture results live on the GPU.
  if (ResolveCurrent(base)) {
    // The render scale above bit 20: shaders take fetch offsets in guest
    // texels (XenosRecomp's getGuestTexture2DDimensions).
    if (auto it = g_resolvedTextures.find(ResolvedKey(base, fetch));
        it != g_resolvedTextures.end()) {
      Transition(*it->second, RenderTextureLayout::SHADER_READ);
      return it->second->descriptor | ((it->second->scale - 1) << 20);
    }
    if (auto it = g_lastResolve.find(base); it != g_lastResolve.end()) {
      Transition(*it->second, RenderTextureLayout::SHADER_READ);
      return it->second->descriptor | ((it->second->scale - 1) << 20);
    }
  }

  TextureFormatInfo info = GetTextureFormatInfo(fetch.format);
  if (info.format == RenderFormat::UNKNOWN) {
    ReportUnsupportedTexture("format", fetch);
    return 0;
  }
  uint32_t width = oneD ? fetch.size_1d.width + 1 : fetch.size_2d.width + 1;
  uint32_t height = oneD ? 1 : fetch.size_2d.height + 1;
  TextureLayout layout = GetTextureLayout(fetch, info, width, height);
  uint64_t guestSize = layout.levels[0].size;
  if (guestSize == 0 || base + guestSize > GUEST_MEMORY_SIZE) {
    ReportUnsupportedTexture("size", fetch);
    return 0;
  }
  if (layout.mipAddress + layout.mipsSize > GUEST_MEMORY_SIZE) {
    layout.levels.resize(1);  // mips past the end of memory: the base alone
    layout.mipsSize = 0;
  }

  // Shape and view: dwords 0-2, the swizzle, the mip levels of dword 4, and
  // the dimension, packed mips bit and mip address of dword 5; sampler fields
  // don't matter.
  uint32_t keyData[6] = {fetch.dword_0 & ~0x3FFFFFu, fetch.dword_1, fetch.dword_2,
                         uint32_t(fetch.swizzle), fetch.dword_4 & 0x3FC,
                         fetch.dword_5 & 0xFFFFFE00};
  uint64_t key = XXH3_64bits(keyData, sizeof(keyData));
  auto& cached = g_textures[key];
  if (cached.checkedFrame != g_frameCount) {
    cached.checkedFrame = g_frameCount;
    const uint8_t* data = guest::Physical(base);
    const uint8_t* mipData = guest::Physical(layout.mipAddress);
    uint64_t hash = XXH3_64bits(data, guestSize) ^
                    (layout.mipsSize ? XXH3_64bits(mipData, layout.mipsSize) * 0x9E3779B97F4A7C15ull
                                     : 0);
    if (!cached.host || hash != cached.contentHash) {
      if (cached.host) {
        g_garbage[g_frame].push_back(std::move(cached.host));
      }
      bool retry = false;  // made differently once another texture is seen
      cached.host =
          CreateTexture(fetch, info, layout, data, guestSize, mipData, layout.mipsSize, &retry);
      cached.contentHash = retry ? 0 : hash;
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
  uint32_t alphaFunc;  // RB_COLORCONTROL alpha_func
  float resolutionScale;  // host pixels per guest pixel (g_resolutionScale)
  float debugColor[4];  // NG2_DEBUG_HIGHLIGHT
};
static_assert(offsetof(SharedConstants, samplerIndices) == 512);
static_assert(offsetof(SharedConstants, booleans) == 640);
static_assert(offsetof(SharedConstants, guestMemoryBase) == 688);
static_assert(offsetof(SharedConstants, vertexFetch) == 696);
static_assert(offsetof(SharedConstants, vertexElements) == 1464);
static_assert(offsetof(SharedConstants, ndcTransform) == 1720);
static_assert(offsetof(SharedConstants, alphaFunc) == 1736);
static_assert(offsetof(SharedConstants, resolutionScale) == 1740);
static_assert(offsetof(SharedConstants, debugColor) == 1744);

// Pipeline state that varies per draw (hashed into the pipeline key).
// The depth bias of a draw (PA_SU_POLY_OFFSET_*) in Vulkan's terms, picked
// as Xenia does (draw_util::GetPreferredFacePolygonOffset): one bias for the
// whole draw, the front face's unless that face is culled. The guest offset
// is in depth units, about 2^-24 each on the host's float buffer near 1; the
// scale is in 1/16 subpixels.
void PolygonOffset(uint32_t device, reg::PA_SU_SC_MODE_CNTL mode, uint32_t primitive,
                   float& constant, float& slope) {
  float scale = 0.0f, offset = 0.0f;
  auto type = xenos::PrimitiveType(primitive);
  bool polygonal = type != xenos::PrimitiveType::kPointList &&
                   type != xenos::PrimitiveType::kLineList &&
                   type != xenos::PrimitiveType::kLineStrip &&
                   type != xenos::PrimitiveType::kLineLoop &&
                   type != xenos::PrimitiveType::kRectangleList;
  if (polygonal) {
    if (mode.poly_offset_front_enable && !mode.cull_front) {
      scale = RegisterFloat(device, 0x2380);
      offset = RegisterFloat(device, 0x2381);
    }
    if (mode.poly_offset_back_enable && !mode.cull_back && scale == 0.0f && offset == 0.0f) {
      scale = RegisterFloat(device, 0x2382);
      offset = RegisterFloat(device, 0x2383);
    }
  } else if (mode.poly_offset_para_enable) {
    scale = RegisterFloat(device, 0x2380);
    offset = RegisterFloat(device, 0x2381);
  }
  // The pipeline takes whole units: rounded away from zero, as Xenia's
  // GetD3D10IntegerPolygonOffset (more bias is better than none).
  constant = std::copysign(std::ceil(std::abs(offset) * float(1u << 24)), offset);
  // Per host pixel the depth slope is smaller by the render scale (Xenia
  // multiplies by its resolution scale too).
  slope = scale * xenos::kPolygonOffsetScaleSubpixelUnit * float(g_resolutionScale);
}

struct PipelineKey {
  uint64_t vertexShader;  // shader cache entry hashes
  uint64_t pixelShader;
  uint32_t primitive;
  uint32_t colorFormats[4];  // RenderFormat::UNKNOWN past the bound targets
  uint32_t depthFormat;
  uint32_t samples;
  uint32_t blendControls[4];
  uint32_t colorMask;  // RB_COLOR_MASK, 4 bits a target, minus unwritten outputs
  uint32_t depthControl;
  uint32_t stencilMasks;
  uint32_t cull;
  uint32_t alphaTest;
  uint32_t alphaToCoverage;
  float depthBias;  // host units (PolygonOffset)
  float slopeDepthBias;
};

RenderPipeline* GetPipeline(const PipelineKey& key, RenderShader* vertexShader,
                            RenderShader* pixelShader) {
  StageTimer timer(Stage::kPipeline);
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
  // Front faces are counter-clockwise unless PA_SU_SC_MODE_CNTL.face (as
  // Xenia sets VkPipelineRasterizationStateCreateInfo.frontFace); plume's
  // pipelines take clockwise as front, so a culled side is the other one.
  if (mode.cull_front && mode.cull_back) {
    desc.cullMode = RenderCullMode::NONE;  // everything culled: drawn as nothing below
  } else if (mode.cull_front) {
    desc.cullMode = mode.face ? RenderCullMode::FRONT : RenderCullMode::BACK;
  } else if (mode.cull_back) {
    desc.cullMode = mode.face ? RenderCullMode::BACK : RenderCullMode::FRONT;
  }
  for (uint32_t i = 0; i < 4 && key.colorFormats[i] != uint32_t(RenderFormat::UNKNOWN); i++) {
    desc.renderTargetFormat[i] = RenderFormat(key.colorFormats[i]);
    desc.renderTargetCount = i + 1;
    reg::RB_BLENDCONTROL blend;
    blend.value = key.blendControls[i];
    auto& target = desc.renderTargetBlend[i];
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
    target.renderTargetWriteMask = uint8_t((key.colorMask >> (i * 4)) & 0xF);
  }
  desc.depthTargetFormat = RenderFormat(key.depthFormat);
  if (desc.depthTargetFormat != RenderFormat::UNKNOWN) {
    reg::RB_DEPTHCONTROL depth;
    depth.value = key.depthControl;
    desc.depthEnabled = depth.z_enable;
    desc.depthWriteEnabled = depth.z_enable && depth.z_write_enable;
    desc.depthFunction = ConvertCompare(depth.zfunc);
    desc.depthBias = int32_t(key.depthBias);
    desc.slopeScaledDepthBias = key.slopeDepthBias;
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
  desc.alphaToCoverageEnabled = key.alphaToCoverage != 0;
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
  HostTexture* targets[4] = {};
  uint32_t targetCount = 0;
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
  BindFramebuffer(record.targets, record.targetCount, record.depth);
  auto* commandList = CommandList();
  commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
  commandList->setPipeline(record.pipeline);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 0);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
  commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 3);
  commandList->setGraphicsPushConstants(0, record.pushConstants, 0, sizeof(record.pushConstants));
  // Plume gives a pipeline one viewport per color target.
  RenderViewport viewports[4];
  RenderRect scissors[4];
  uint32_t viewportCount = std::max(record.targetCount, 1u);
  std::fill_n(viewports, viewportCount, record.viewport);
  std::fill_n(scissors, viewportCount, record.scissor);
  commandList->setViewports(viewports, viewportCount);
  commandList->setScissors(scissors, viewportCount);
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
  SyncGuestMemory(PhysicalAddress(data.vertexAddress), data.vertexBytes, true);
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
  StageTimer timer(Stage::kDraw);
  auto findShader = [](uint32_t object) -> GuestShader* {
    auto it = g_guestShaders.find(object);
    return it != g_guestShaders.end() ? it->second.get() : nullptr;
  };
  GuestShader* vs = findShader(guest::Load32(device + 0x3190));
  GuestShader* ps = findShader(guest::Load32(device + 0x318C));
  RenderShader* vertexShader = vs ? GetHostShader(*vs) : nullptr;
  RenderShader* pixelShader = ps ? GetHostShader(*ps) : nullptr;
  // No pixel shader: a depth-only draw (Z prepass, shadow casters).
  bool depthOnly = guest::Load32(device + 0x318C) == 0;
  if (!vertexShader || (!pixelShader && !depthOnly) || count == 0) {
    return;
  }

  // Bound color targets from 0 up, while they match target 0's shape (one
  // framebuffer).
  HostTexture* targets[4] = {};
  uint32_t targetCount = 0;
  for (uint32_t i = 0; i < 4; i++) {
    HostTexture* surface =
        GetSurface(guest::Load32(device + guest::device::kRenderTargets + i * 4), false);
    if (!surface || (i > 0 && (surface->width != targets[0]->width ||
                               surface->height != targets[0]->height ||
                               surface->samples != targets[0]->samples))) {
      break;
    }
    targets[targetCount++] = surface;
  }
  HostTexture* target = targets[0];
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
    if (slot == kBloomSlot && ps &&
        (ps->entry->hash == kToneMapPixelShader || ps->entry->hash == kCompositePixelShader)) {
      descriptor = AdjustBloom(fetch, descriptor);
    }
    if (Tracing()) {
      REXLOG_INFO("  tex {} fmt {} {}x{} base {:#x} tiled {} endian {} dim {} sign {}{}{}{} "
                  "swizzle {:#o} exp {} -> {}",
                  slot, uint32_t(fetch.format), fetch.size_2d.width + 1,
                  fetch.size_2d.height + 1, uint32_t(fetch.base_address) << 12,
                  uint32_t(fetch.tiled), uint32_t(fetch.endianness), uint32_t(fetch.dimension),
                  uint32_t(fetch.sign_x), uint32_t(fetch.sign_y), uint32_t(fetch.sign_z),
                  uint32_t(fetch.sign_w), uint32_t(fetch.swizzle), int32_t(fetch.exp_adjust),
                  descriptor);
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
  shared.alphaFunc = uint32_t(colorControl.alpha_func);
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

  // Declared vertex elements, from the vertex declaration.
  // Pixel shaders can fetch vertex data too.
  uint32_t fetchMask[3];
  for (int i = 0; i < 3; i++) {
    fetchMask[i] = vs->entry->vertexFetchMask[i] | (ps ? ps->entry->vertexFetchMask[i] : 0);
  }
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
        uint32_t fetchSlot = 95 - std::min(stream, 95u);
        shared.vertexElements[slot][0] = fetchSlot;
        shared.vertexElements[slot][1] = offset / 4;
        shared.vertexElements[slot][2] = stride;
        shared.vertexElements[slot][3] = ElementFlags(type);
        fetchMask[fetchSlot / 32] |= 1u << (fetchSlot % 32);
        break;
      }
    }
  }

  // Vertex data the shader reads (inline data is synced when it has been
  // written).
  for (uint32_t i = 0; i < (inlineData ? 95u : 96u); i++) {
    if (!(fetchMask[i / 32] & (1u << (i % 32)))) {
      continue;
    }
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

  // Viewport: hardware transform, or positions in pixels.
  reg::PA_CL_VTE_CNTL vte;
  vte.value = Register(device, 0x2206);
  HostTexture* sizeSource = target ? target : depth;
  float targetWidth = float(sizeSource->width);
  float targetHeight = float(sizeSource->height);
  // The guest's viewport is in guest pixels; the target has scale host
  // pixels for each.
  float scale = float(sizeSource->scale);
  shared.resolutionScale = scale;
  // D3D's pixel centers (PA_SU_VTX_CNTL.pix_center 0) are at integer
  // coordinates: shift everything by half a guest pixel, as Xenia does, or
  // full-screen passes sample between texels and blurs drift.
  reg::PA_SU_VTX_CNTL vtxControl;
  vtxControl.value = Register(device, 0x2302);
  float halfPixel = REXCVAR_GET(half_pixel_offset) &&
                            vtxControl.pix_center == xenos::PixelCenter::kD3DZero
                        ? 0.5f
                        : 0.0f;
  RenderViewport viewport(0.0f, 0.0f, targetWidth, targetHeight, 0.0f, 1.0f);
  if (vte.vport_x_scale_ena) {
    float xScale = RegisterFloat(device, 0x210F) * scale;
    float xOffset = RegisterFloat(device, 0x2110) * scale;
    float yScale = RegisterFloat(device, 0x2111) * scale;
    float yOffset = RegisterFloat(device, 0x2112) * scale;
    float zScale = RegisterFloat(device, 0x2113), zOffset = RegisterFloat(device, 0x2114);
    viewport = RenderViewport(xOffset - std::fabs(xScale), yOffset - std::fabs(yScale),
                              2.0f * std::fabs(xScale), 2.0f * std::fabs(yScale),
                              // Window z = zOffset + z * zScale, as on Xenos: a
                              // negative scale (reversed depth) stays reversed,
                              // which Vulkan allows (minDepth > maxDepth).
                              zOffset, zOffset + zScale);
    // The host viewport is always top-down with positive sizes. D3D's usual
    // negative Y scale is what -fvk-invert-y (on every translated vertex
    // shader) expects; mirror only the unusual signs.
    shared.ndcTransform[0] = xScale < 0 ? -1.0f : 1.0f;
    shared.ndcTransform[1] = yScale < 0 ? 1.0f : -1.0f;
    // The half pixel moves the geometry, not the viewport (which clips): as
    // Xenia's ndc_offset. A host pixel is 1/|scale| in NDC; y is negated by
    // the shaders' -fvk-invert-y after this transform.
    if (xScale != 0.0f && yScale != 0.0f) {
      shared.ndcTransform[2] = halfPixel * scale / std::fabs(xScale);
      shared.ndcTransform[3] = -halfPixel * scale / std::fabs(yScale);
    }
  } else {
    // Window coordinates (guest pixels): map them to NDC over the target.
    shared.ndcTransform[0] = 2.0f * scale / targetWidth;
    shared.ndcTransform[1] = -2.0f * scale / targetHeight;
    shared.ndcTransform[2] = -1.0f + halfPixel * shared.ndcTransform[0];
    shared.ndcTransform[3] = 1.0f + halfPixel * shared.ndcTransform[1];
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
  key.pixelShader = ps ? ps->entry->hash : 1;  // 0 is the debug shader's
  key.primitive = primitive;
  for (uint32_t i = 0; i < 4; i++) {
    key.colorFormats[i] =
        uint32_t(i < targetCount ? targets[i]->format : RenderFormat::UNKNOWN);
  }
  key.depthFormat = uint32_t(depth ? depth->format : RenderFormat::UNKNOWN);
  key.samples = uint32_t(sizeSource->samples);
  static constexpr uint32_t kBlendControls[4] = {0x2201, 0x2209, 0x220A, 0x220B};
  for (uint32_t i = 0; i < targetCount; i++) {
    key.blendControls[i] = Register(device, kBlendControls[i]);
  }
  // A target the pixel shader doesn't write keeps its contents.
  uint32_t written = 0;
  for (uint32_t i = 0; i < 4; i++) {
    written |= (ps && (ps->colorOutputs >> i & 1)) ? 0xFu << (i * 4) : 0;
  }
  key.colorMask = Register(device, 0x2104) & written & ((1u << (targetCount * 4)) - 1);
  key.depthControl = Register(device, 0x2200);
  key.stencilMasks = Register(device, 0x210D);
  key.cull = mode.value & 0x7;
  PolygonOffset(device, mode, primitive, key.depthBias, key.slopeDepthBias);
  key.alphaTest = colorControl.alpha_test_enable;
  key.alphaToCoverage = ps && colorControl.alpha_to_mask_enable;  // needs a fragment shader
  // =all colors the skinned draws only (bones in fetch slot 94): other
  // shaders include full-screen passes that would cover them.
  bool skinned = (vs->entry->vertexFetchMask[2] >> 30) & 1;
  if (Highlighted(vs->entry->hash) &&
      (skinned || std::strcmp(std::getenv("NG2_DEBUG_HIGHLIGHT"), "all") != 0)) {
    key.pixelShader = 0;  // the debug shader's pipelines
    pixelShader = g_debugShader.get();
    DebugColor(vs->entry->hash, reinterpret_cast<SharedConstants*>(sharedUpload.data)->debugColor);
  }
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
                ps ? ps->container : 0);
    REXLOG_INFO("draw prim {} count {} indexed {} vs {:016X} ps {:016X} target {}x{} fmt {} "
                "targets {} mask {:#x} blend {:#x} depth {:#x} vte {:#x} vp {} {} {} {} "
                "base {} indx_offset {:#x} min/max {:#x}/{:#x} colorcontrol {:#x} bools {:08x} "
                "bias {} {}",
                primitive, count, indexed, vs->entry->hash, ps ? ps->entry->hash : 0,
                sizeSource->width, sizeSource->height, uint32_t(sizeSource->format), targetCount,
                key.colorMask, key.blendControls[0], key.depthControl, vte.value, viewport.x, viewport.y,
                viewport.width, viewport.height, startVertex, Register(device, 0x2102),
                Register(device, 0x2101), Register(device, 0x2100), colorControl.value,
                shared.booleans[4], key.depthBias, key.slopeDepthBias);
  }
  DrawRecord record;
  record.pipeline = pipeline;
  record.memExport = memExport;
  std::copy(targets, targets + targetCount, record.targets);
  record.targetCount = targetCount;
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
  StageTimer timer(Stage::kPresent);
  auto* commandList = CommandList();
  FinishGuestMemory();
  if (Tracing()) {
    REXLOG_INFO("present frame {}", g_frameCount);
  }

  g_swapChain->setVsyncEnabled(REXCVAR_GET(vsync));  // takes effect through a resize
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
      uint32_t constants[3] = {frontBuffer->descriptor, SAMPLER_LINEAR_CLAMP,
                               g_gammaTable ? g_gammaTable->descriptor : 0u};
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
  g_copy7e3Shader = CreateShader(g_copy_7e3_ps, g_copy_7e3_ps_size);
  g_copy7e3MsShader = CreateShader(g_copy_7e3_ms_ps, g_copy_7e3_ms_ps_size);
  g_scaleShader = CreateShader(g_scale_ps, g_scale_ps_size);
  g_debugShader = CreateShader(g_debug_ps, g_debug_ps_size);
  g_depthCopyShader = CreateShader(g_depth_copy_ps, g_depth_copy_ps_size);
  g_depthCopyMsShader = CreateShader(g_depth_copy_ms_ps, g_depth_copy_ms_ps_size);

  g_guestMemory = g_device->createBuffer(RenderBufferDesc::DefaultBuffer(
      GUEST_MEMORY_SIZE, RenderBufferFlag::CONSTANT | RenderBufferFlag::STORAGE));
  g_guestMemoryAddress = g_guestMemory->getDeviceAddress();
  g_guestMemoryShadow = static_cast<uint8_t*>(std::calloc(GUEST_MEMORY_SIZE, 1));
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
  g_resolutionScale = uint32_t(std::clamp(REXCVAR_GET(render_scale), 1, 4));
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
  g_frameRanges.clear();
  std::free(g_guestMemoryShadow);
  g_guestMemoryShadow = nullptr;
  g_guestMemoryRanges.clear();
  g_resolveTemps.clear();
  g_copyPipelines.clear();
  g_copyShader.reset();
  g_copy7e3Shader.reset();
  g_copy7e3MsShader.reset();
  g_scaleShader.reset();
  g_reducedBloom.reset();
  g_rectListShader.reset();
  g_framebuffers.clear();
  g_surfaces.clear();
  g_gammaTable.reset();
  g_lastResolve.clear();
  g_resolvedTextures.clear();
  g_resolvedMemory.clear();
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
  if (!clearColor && !(flags & 0xE) && !clearDepth && !clearStencil) {
    return;
  }
  if (!target && !depth) {
    return;  // targets 1-3 without 0: rects can't be clipped; not seen
  }
  // Attachments of different shapes can't share a framebuffer.
  bool together = target && depth && target->width == depth->width &&
                  target->height == depth->height && target->samples == depth->samples;

  // Rects clipped to the target; with rects given but none usable, nothing
  // to clear.
  int32_t limitX = int32_t(target ? target->width : depth->width);
  int32_t limitY = int32_t(target ? target->height : depth->height);
  int32_t scale = int32_t(target ? target->scale : depth->scale);  // guest to host pixels
  std::vector<RenderRect> clearRects;
  for (uint32_t i = 0; i < rectCount; i++) {
    // Clamped in guest pixels first: huge rects would overflow when scaled.
    int32_t guestX = limitX / scale, guestY = limitY / scale;
    RenderRect rect(std::clamp(rects[i][0], 0, guestX) * scale,
                    std::clamp(rects[i][1], 0, guestY) * scale,
                    std::clamp(rects[i][2], 0, guestX) * scale,
                    std::clamp(rects[i][3], 0, guestY) * scale);
    if (rect.right > rect.left && rect.bottom > rect.top) {
      clearRects.push_back(rect);
    }
  }
  if (rectCount && clearRects.empty()) {
    return;
  }
  const RenderRect* rectData = clearRects.empty() ? nullptr : clearRects.data();
  uint32_t rectDataCount = uint32_t(clearRects.size());

  RenderColor value(float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f,
                    float(color & 0xFF) / 255.0f, float(color >> 24) / 255.0f);
  if (clearColor) {
    BindFramebuffer(target, together ? depth : nullptr);
    CommandList()->clearColor(0, value, rectData, rectDataCount);
  }
  if (clearDepth || clearStencil) {
    if (!(clearColor && together)) {
      BindFramebuffer(nullptr, depth);
    }
    CommandList()->clearDepthStencil(clearDepth, clearStencil, z, stencil & 0xFF, rectData,
                                     rectDataCount);
  }
  for (uint32_t i = 1; i < 4; i++) {
    HostTexture* other =
        (flags & (1u << i))
            ? GetSurface(guest::Load32(device + guest::device::kRenderTargets + i * 4), false)
            : nullptr;
    if (other && (!rectDataCount || (other->width == uint32_t(limitX) &&
                                     other->height == uint32_t(limitY)))) {
      BindFramebuffer(other, nullptr);
      CommandList()->clearColor(0, value, rectData, rectDataCount);
    }
  }
}

void OnResolve(uint32_t device, uint32_t flags, uint32_t sourceRect, uint32_t destTexture,
               uint32_t destPoint, uint32_t clearColor, float clearZ, uint32_t clearStencil) {
  std::lock_guard lock(g_mutex);
  StageTimer timer(Stage::kResolve);
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

  if (target && destTexture) {
    auto fetch = ReadTextureFetch(destTexture);
    uint32_t address = PhysicalAddress(uint32_t(fetch.base_address) << 12);
    HostTexture* dest = GetResolveDestination(address, fetch, target->scale);
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
      // Guest pixels to the targets' host pixels.
      if (sourceRect) {
        int32_t scale = int32_t(target->scale);
        x1 *= scale, y1 *= scale, x2 *= scale, y2 *= scale;
      }
      dx *= target->scale, dy *= target->scale;
      if (dx < dest->width && dy < dest->height) {
        x1 = std::max(x1, 0);
        y1 = std::max(y1, 0);
        x2 = std::min({x2, int32_t(target->width), x1 + int32_t(dest->width - dx)});
        y2 = std::min({y2, int32_t(target->height), y1 + int32_t(dest->height - dy)});
      } else {
        x2 = x1;
      }
      if (depth && dest->format != RenderFormat::R32_FLOAT) {
        static bool warned = false;
        if (!warned) {
          warned = true;
          REXLOG_WARN("video: depth resolved into texture format {}, not converted",
                      uint32_t(fetch.format));
        }
      }
      if (x2 > x1 && y2 > y1 && (depth || target->float7e3 || dest->format != target->format)) {
        ResolveByDraw(*target, *dest, x1, y1, x2, y2, dx, dy, depth);
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
  if (pixel) {
    uint32_t header = container + guest::Load32(container + 24);
    shader->colorOutputs = guest::Load32(header + 28) & 0xF;
  } else {
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

// The display's gamma table, as D3D's SetGammaRamp leaves it at dev+0x3A8C:
// 256 16-bit entries for red, then green, then blue (DC_LUT_SEQ_COLOR, low
// 6 bits zero), already adjusted for the display gamma the kernel reports
// (VdGetCurrentDisplayGamma, BT.709). The console applies it on scan-out, as
// Xenia's presenter does; even an identity game ramp darkens the picture.
// Kept as a 256x1 texture for the present shader.
void UpdateGammaTable(uint32_t device) {
  // With a 10-bit front buffer (D3DFORMAT 0x28280136 at dev+0x3518) D3D keeps
  // the same bytes as 128 PWL {base, delta} pairs instead: not handled yet.
  if (guest::Load32(device + 0x3518) == 0x28280136) {
    if (g_gammaTable) {
      g_garbage[g_frame].push_back(std::move(g_gammaTable));  // a frame may still read it
    }
    return;
  }
  uint16_t table[3][256];
  bool any = false;
  for (uint32_t c = 0; c < 3; c++) {
    for (uint32_t i = 0; i < 256; i++) {
      table[c][i] = guest::Load16(device + 0x3A8C + (c * 256 + i) * 2);
      any |= table[c][i] != 0;
    }
  }
  if (!any) {
    return;  // not set up yet: present unchanged
  }
  uint64_t hash = XXH3_64bits(table, sizeof(table));
  if (g_gammaTable && hash == g_gammaTableHash) {
    return;
  }
  g_gammaTableHash = hash;
  if (!g_gammaTable) {
    g_gammaTable = std::make_unique<HostTexture>();
    g_gammaTable->width = 256;
    g_gammaTable->height = 1;
    g_gammaTable->format = RenderFormat::R16G16B16A16_UNORM;
    g_gammaTable->texture = g_device->createTexture(
        RenderTextureDesc::Texture2D(256, 1, 1, RenderFormat::R16G16B16A16_UNORM));
    SetDescriptor(*g_gammaTable);
  }
  Upload upload = Allocate(256 * 8, 16);
  auto* texels = reinterpret_cast<uint16_t*>(upload.data);
  for (uint32_t i = 0; i < 256; i++) {
    // 10-bit values in the high bits: 1023 << 6 must read as 1.0.
    for (uint32_t c = 0; c < 3; c++) {
      texels[i * 4 + c] = uint16_t(table[c][i] | (table[c][i] >> 10));
    }
    texels[i * 4 + 3] = 0xFFFF;
  }
  Transition(*g_gammaTable, RenderTextureLayout::COPY_DEST);
  CommandList()->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(g_gammaTable->texture.get()),
      RenderTextureCopyLocation::PlacedFootprint(upload.buffer, RenderFormat::R16G16B16A16_UNORM,
                                                 256, 1, 1, 256, upload.offset));
  Transition(*g_gammaTable, RenderTextureLayout::SHADER_READ);
}

// A new render_scale, between frames: the render targets are made again at
// the new size (their contents start over).
void ApplyRenderScale() {
  uint32_t scale = uint32_t(std::clamp(REXCVAR_GET(render_scale), 1, 4));
  if (scale == g_resolutionScale) {
    return;
  }
  WaitForGPU();
  for (auto& [key, texture] : g_surfaces) {
    g_descriptors.Free(texture->descriptor);
  }
  for (auto& [source, texture] : g_resolveTemps) {
    g_descriptors.Free(texture->descriptor);
  }
  g_framebuffers.clear();
  g_boundFramebuffer = nullptr;
  g_surfaces.clear();
  g_resolveTemps.clear();
  // Resolved textures stay (guest memory never got their pixels); each is
  // made again at the new scale by its next resolve (GetResolveDestination).
  g_resolutionScale = scale;
  REXLOG_INFO("video: render scale {}", scale);
}

void OnSwap(uint32_t device, uint32_t frontBuffer) {
  std::lock_guard lock(g_mutex);
  if (!g_device) {
    return;
  }
  FlushPendingDraw();
  UpdateGammaTable(device);
  HostTexture* front = nullptr;
  if (frontBuffer) {
    auto fetch = ReadTextureFetch(frontBuffer);
    uint32_t address = PhysicalAddress(uint32_t(fetch.base_address) << 12);
    if (auto it = g_resolvedTextures.find(ResolvedKey(address, fetch));
        it != g_resolvedTextures.end()) {
      front = it->second.get();
    } else if (auto last = g_lastResolve.find(address); last != g_lastResolve.end()) {
      front = last->second;
    }
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("video: front buffer {:#x} {}", uint32_t(fetch.base_address) << 12,
                  front ? "found" : "not resolved");
    }
  }
  Present(front);
  ApplyRenderScale();
  ReportProfile();
}

}  // namespace ng2::video
