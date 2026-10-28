// ng2 - Ninja Gaiden II static recompilation
//
// Hooks on the game's D3D library (XDK 2.0.6683; addresses and signatures in
// docs/d3d-map-*.md). With the native renderer the original still runs, so
// the D3D library's bookkeeping and command stream stay intact (the GPU
// plugin's null backend retires that stream); the renderer reads the device
// state at these points. Otherwise they only call the original.

#include <algorithm>
#include <atomic>
#include <chrono>

#include <rex/hook.h>
#include <rex/logging.h>

#include "guest.h"
#include "video.h"

namespace {

struct Counters {
  std::atomic<uint32_t> draws{0};
  std::atomic<uint32_t> clears{0};
  std::atomic<uint32_t> resolves{0};
  std::atomic<uint32_t> swaps{0};
};
Counters g_counters;
std::chrono::steady_clock::time_point g_lastReport = std::chrono::steady_clock::now();

// NG2_D3D_STATS=1: log D3D call counts once a second.
bool StatsEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("NG2_D3D_STATS");
    return v && v[0] == '1';
  }();
  return enabled;
}

void ReportStats() {
  auto now = std::chrono::steady_clock::now();
  if (now - g_lastReport < std::chrono::seconds(1)) {
    return;
  }
  g_lastReport = now;
  REXLOG_INFO("d3d: {} swaps, {} draws, {} clears, {} resolves in the last second",
              g_counters.swaps.exchange(0), g_counters.draws.exchange(0),
              g_counters.clears.exchange(0), g_counters.resolves.exchange(0));
}

}  // namespace

// DrawVertices(dev, prim type, start vertex, vertex count)
REX_EXTERN(__imp__sub_8373C720);
REX_HOOK_RAW(sub_8373C720) {
  g_counters.draws++;
  __imp__sub_8373C720(ctx, base);
}

// DrawIndexedVertices(dev, prim type, base vertex, start index, index count)
REX_EXTERN(__imp__sub_8373CB08);
REX_HOOK_RAW(sub_8373CB08) {
  g_counters.draws++;
  __imp__sub_8373CB08(ctx, base);
}

// BeginVertices(dev, prim type, vertex count, stride) -> vertex data
REX_EXTERN(__imp__sub_8373BD50);
REX_HOOK_RAW(sub_8373BD50) {
  g_counters.draws++;
  __imp__sub_8373BD50(ctx, base);
}

// BeginIndexedVertices(dev, prim type, base vertex, vertex count, index
// count, index format, stride, &indices, [stack] &vertices)
REX_EXTERN(__imp__sub_8373C1F0);
REX_HOOK_RAW(sub_8373C1F0) {
  g_counters.draws++;
  __imp__sub_8373C1F0(ctx, base);
}

// Clear(dev, count, rects, flags, color, f1 z, stencil)
REX_EXTERN(__imp__sub_837390C8);
REX_HOOK_RAW(sub_837390C8) {
  g_counters.clears++;
  // The float takes r8's slot, so stencil comes in r9.
  uint32_t device = ctx.r3.u32, count = ctx.r4.u32, rects = ctx.r5.u32, flags = ctx.r6.u32;
  uint32_t color = ctx.r7.u32, stencil = ctx.r9.u32;
  float z = float(ctx.f1.f64);
  // The game builds the rects below its stack pointer, where the call itself
  // writes: copy them first.
  int32_t rectData[8][4];
  count = rects ? std::min(count, 8u) : 0;
  for (uint32_t i = 0; i < count; i++) {
    for (uint32_t j = 0; j < 4; j++) {
      rectData[i][j] = ng2::guest::LoadS32(rects + i * 16 + j * 4);
    }
  }
  __imp__sub_837390C8(ctx, base);
  if (ng2::video::IsNativeRenderer()) {
    ng2::video::OnClear(device, count, rectData, flags, color, z, stencil);
  }
}

// Resolve(dev, flags, src rect, dest texture, dest point, level, slice,
// clear color, f1 clear z, [stack] stencil, params)
REX_EXTERN(__imp__sub_8373D570);
REX_HOOK_RAW(sub_8373D570) {
  g_counters.resolves++;
  uint32_t device = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, texture = ctx.r6.u32;
  uint32_t point = ctx.r7.u32, clearColor = ctx.r10.u32;
  float clearZ = float(ctx.f1.f64);
  // 10th argument on the caller's stack (the float takes the 9th slot).
  uint32_t clearStencil = ng2::guest::Load32(ctx.r1.u32 + 92);
  __imp__sub_8373D570(ctx, base);
  if (ng2::video::IsNativeRenderer()) {
    ng2::video::OnResolve(device, flags, rect, texture, point, clearColor, clearZ, clearStencil);
  }
}

// Swap: the frame is done.
REX_EXTERN(__imp__sub_837452C0);
REX_HOOK_RAW(sub_837452C0) {
  g_counters.swaps++;
  uint32_t device = ctx.r3.u32, frontBuffer = ctx.r4.u32;
  __imp__sub_837452C0(ctx, base);
  if (ng2::video::IsNativeRenderer()) {
    ng2::video::OnSwap(device, frontBuffer);
  }
  if (StatsEnabled()) {
    ReportStats();
  }
}
