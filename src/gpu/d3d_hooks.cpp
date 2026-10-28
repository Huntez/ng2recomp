// ng2 - Ninja Gaiden II static recompilation
//
// Hooks on the game's D3D library (XDK 2.0.6683; addresses and signatures in
// docs/d3d-map-*.md). With the native renderer the original still runs, so
// the D3D library's bookkeeping and command stream stay intact (the GPU
// plugin's null backend retires that stream); the renderer reads the device
// state at these points. Otherwise they only call the original.

#include <atomic>
#include <chrono>

#include <rex/hook.h>
#include <rex/logging.h>

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
  __imp__sub_837390C8(ctx, base);
}

// Resolve(dev, flags, src rect, dest texture, dest point, level, slice,
// clear color, f1 clear z, [stack] stencil, params)
REX_EXTERN(__imp__sub_8373D570);
REX_HOOK_RAW(sub_8373D570) {
  g_counters.resolves++;
  __imp__sub_8373D570(ctx, base);
}

// Swap: the frame is done.
REX_EXTERN(__imp__sub_837452C0);
REX_HOOK_RAW(sub_837452C0) {
  g_counters.swaps++;
  __imp__sub_837452C0(ctx, base);
  if (StatsEnabled()) {
    ReportStats();
  }
}
