// ng2 - Ninja Gaiden II static recompilation
//
// Native renderer, phase 1: Vulkan device, swapchain on the game window and a
// present loop. The structure (frames in flight, acquire/render semaphores,
// swapchain barriers) follows Unleashed Recompiled's gpu/video.cpp.

#include "video.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#include <SDL3/SDL.h>

#include <rex/logging.h>

#include <plume_render_interface.h>

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

using namespace plume;

namespace ng2::video {

namespace {

constexpr size_t NUM_FRAMES = 2;
constexpr RenderFormat BACKBUFFER_FORMAT = RenderFormat::B8G8R8A8_UNORM;

std::unique_ptr<RenderInterface> g_interface;
std::unique_ptr<RenderDevice> g_device;
std::unique_ptr<RenderCommandQueue> g_queue;
std::unique_ptr<RenderCommandList> g_commandLists[NUM_FRAMES];
std::unique_ptr<RenderCommandFence> g_commandFences[NUM_FRAMES];
std::unique_ptr<RenderCommandSemaphore> g_acquireSemaphores[NUM_FRAMES];
// Signalled when a swapchain image is rendered, waited on by its present.
// One per swapchain image: a present can still hold its semaphore when the
// next frame in flight starts.
std::vector<std::unique_ptr<RenderCommandSemaphore>> g_renderSemaphores;
bool g_commandListPending[NUM_FRAMES];
std::unique_ptr<RenderSwapChain> g_swapChain;
std::vector<std::unique_ptr<RenderFramebuffer>> g_swapChainFramebuffers;
bool g_swapChainValid = false;
uint32_t g_frame = 0;

std::thread g_presentThread;
std::atomic<bool> g_running{false};

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

// One frame: acquire, clear the swapchain image, present.
void PresentFrame() {
  WaitForFrame(g_frame);

  g_swapChainValid &= !g_swapChain->needsResize();
  if (!g_swapChainValid) {
    WaitForGPU();
    g_swapChainFramebuffers.clear();
    g_swapChainValid = g_swapChain->resize();
  }

  uint32_t index = 0;
  if (g_swapChainValid) {
    g_swapChainValid = g_swapChain->acquireTexture(g_acquireSemaphores[g_frame].get(), &index);
  }

  while (g_renderSemaphores.size() < g_swapChain->getTextureCount()) {
    g_renderSemaphores.push_back(g_device->createCommandSemaphore());
  }

  auto& commandList = g_commandLists[g_frame];
  commandList->begin();

  if (g_swapChainValid) {
    g_swapChainFramebuffers.resize(g_swapChain->getTextureCount());
    RenderTexture* texture = g_swapChain->getTexture(index);
    auto& framebuffer = g_swapChainFramebuffers[index];
    if (!framebuffer) {
      const RenderTexture* attachments[] = {texture};
      RenderFramebufferDesc desc;
      desc.colorAttachments = attachments;
      desc.colorAttachmentsCount = 1;
      framebuffer = g_device->createFramebuffer(desc);
    }

    commandList->barriers(RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(texture, RenderTextureLayout::COLOR_WRITE));
    commandList->setFramebuffer(framebuffer.get());
    commandList->clearColor(0, RenderColor(0.10f, 0.02f, 0.02f, 1.0f));
    commandList->barriers(RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(texture, RenderTextureLayout::PRESENT));
  }

  commandList->end();

  if (g_swapChainValid) {
    const RenderCommandList* commandLists[] = {commandList.get()};
    RenderCommandSemaphore* waitSemaphores[] = {g_acquireSemaphores[g_frame].get()};
    RenderCommandSemaphore* signalSemaphores[] = {g_renderSemaphores[index].get()};
    g_queue->executeCommandLists(commandLists, 1, waitSemaphores, 1, signalSemaphores, 1,
                                 g_commandFences[g_frame].get());
    g_commandListPending[g_frame] = true;
    g_swapChainValid = g_swapChain->present(index, signalSemaphores, 1);
  } else {
    g_queue->executeCommandLists(commandList.get(), g_commandFences[g_frame].get());
    g_commandListPending[g_frame] = true;
  }

  g_frame = (g_frame + 1) % NUM_FRAMES;
}

}  // namespace

bool IsNativeRenderer() {
  const char* renderer = std::getenv("NG2_RENDERER");
  return renderer && std::strcmp(renderer, "native") == 0;
}

bool CreateHostDevice() {
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
  const auto& description = g_device->getDescription();
  REXLOG_INFO("video: Vulkan device '{}'", description.name);

  g_queue = g_device->createCommandQueue(RenderCommandListType::DIRECT);
  for (size_t i = 0; i < NUM_FRAMES; i++) {
    g_commandLists[i] = g_queue->createCommandList();
    g_commandFences[i] = g_device->createCommandFence();
    g_acquireSemaphores[i] = g_device->createCommandSemaphore();
  }

  g_swapChain = g_queue->createSwapChain(renderWindow, 3, BACKBUFFER_FORMAT, 1);
  if (!g_swapChain) {
    REXLOG_ERROR("video: cannot create the swapchain");
    return false;
  }
  g_swapChain->setVsyncEnabled(true);
  g_swapChainValid = !g_swapChain->needsResize();

  // Until the game's Present is hooked, present on our own thread.
  g_running = true;
  g_presentThread = std::thread([] {
    while (g_running) {
      PresentFrame();
    }
  });
  return true;
}

void DestroyHostDevice() {
  if (g_presentThread.joinable()) {
    g_running = false;
    g_presentThread.join();
  }
  if (g_queue) {
    WaitForGPU();
  }
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

}  // namespace ng2::video
