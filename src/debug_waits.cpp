// ng2 - Ninja Gaiden II static recompilation
//
// NG2_DEBUG_WAITS=1: a debugging aid for stalls. Reports guest waits
// (WaitForSingleObjectEx wrapper 8383E698) still blocked after 5 s or that
// took over 2 s, with the handle's last SetEvent caller, failed semaphore
// releases, and the state of the game's job system (control block
// 83C07CF0: submit 822F2688, worker run 822F2870), which is how the
// opening's stall (a lost semaphore wake-up in ReXGlue) was found.

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>

#include "gpu/guest.h"

namespace {

bool Enabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("NG2_DEBUG_WAITS");
    return v && v[0] == '1';
  }();
  return enabled;
}

struct LastSet {
  uint32_t caller = 0;
  uint64_t count = 0;
  std::chrono::steady_clock::time_point when;
};
std::mutex g_mutex;
std::unordered_map<uint32_t, LastSet> g_sets;

struct Waiting {
  uint32_t handle, caller;
  std::chrono::steady_clock::time_point start;
  bool reported = false;
};
std::unordered_map<uint64_t, Waiting> g_waiting;  // by wait id
uint64_t g_submits = 0, g_runs = 0;
std::string g_runLog[8];
uint64_t g_runLogNext = 0;
uint32_t g_lastRunner = 0;
std::chrono::steady_clock::time_point g_lastSubmit, g_lastRun;
uint64_t g_nextWait = 0;

// Every 5 s: waits still blocked after 5 s, once each.
void Watchdog() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::lock_guard lock(g_mutex);
    auto now = std::chrono::steady_clock::now();
    for (auto& [id, wait] : g_waiting) {
      if (wait.reported || now - wait.start < std::chrono::seconds(5)) {
        continue;
      }
      wait.reported = true;
      auto& set = g_sets[wait.handle];
      REXLOG_WARN("wait: blocked on handle {:#x} from {:#x}; last set from {:#x} ({} sets)",
                  wait.handle, wait.caller, set.caller, set.count);
      std::string jobs;
      for (uint32_t i = 0; i < 12; i++) {
        jobs += fmt::format(" {:08x}", ng2::guest::Load32(0x83C07CF0 + i * 4));
      }
      REXLOG_WARN("wait: job control 83C07CF0:{}", jobs);
      auto ms = [&](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count();
      };
      REXLOG_WARN("wait: submits {} (last {} ms ago), runs {} (last {} ms ago, worker r3 {:#x})",
                  g_submits, ms(g_lastSubmit), g_runs, ms(g_lastRun), g_lastRunner);
      for (int i = 0; i < 8; i++) {
        REXLOG_WARN("wait: run {}", g_runLog[(g_runLogNext + i) % 8]);
      }
    }
  }
}

}  // namespace

// SetEvent(handle)
REX_EXTERN(__imp__sub_83833380);
REX_HOOK_RAW(sub_83833380) {
  if (Enabled()) {
    std::lock_guard lock(g_mutex);
    auto& set = g_sets[ctx.r3.u32];
    set.caller = uint32_t(ctx.lr);
    set.count++;
    set.when = std::chrono::steady_clock::now();
  }
  __imp__sub_83833380(ctx, base);
}

// WaitForSingleObjectEx(handle, timeout, alertable)
REX_EXTERN(__imp__sub_8383E698);
REX_HOOK_RAW(sub_8383E698) {
  if (!Enabled()) {
    __imp__sub_8383E698(ctx, base);
    return;
  }
  uint32_t handle = ctx.r3.u32, timeout = ctx.r4.u32, caller = uint32_t(ctx.lr);
  auto start = std::chrono::steady_clock::now();
  uint64_t id;
  {
    std::lock_guard lock(g_mutex);
    static std::once_flag once;
    std::call_once(once, [] { std::thread(Watchdog).detach(); });
    id = g_nextWait++;
    g_waiting[id] = {handle, caller, start};
  }
  __imp__sub_8383E698(ctx, base);
  {
    std::lock_guard lock(g_mutex);
    g_waiting.erase(id);
  }
  auto waited = std::chrono::steady_clock::now() - start;
  if (waited > std::chrono::seconds(2)) {
    std::lock_guard lock(g_mutex);
    auto& set = g_sets[handle];
    REXLOG_WARN("wait: handle {:#x} timeout {:#x} from {:#x} took {} ms; last set from {:#x} "
                "({} sets, {} ms before the wait ended)",
                handle, timeout, caller,
                std::chrono::duration_cast<std::chrono::milliseconds>(waited).count(), set.caller,
                set.count,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - set.when)
                    .count());
  }
}

// ReleaseSemaphore(handle, count, &previous): log failures.
REX_EXTERN(__imp__sub_83833948);
REX_HOOK_RAW(sub_83833948) {
  uint32_t handle = ctx.r3.u32, count = ctx.r4.u32, lr = uint32_t(ctx.lr);
  __imp__sub_83833948(ctx, base);
  if (Enabled() && ctx.r3.u32 == 0) {
    static int reports = 0;
    if (reports++ < 10) {
      REXLOG_WARN("wait: ReleaseSemaphore({:#x}, {}) from {:#x} failed", handle, count, lr);
    }
  }
}

// The job system: submit (822F2688) and a worker's run (822F2870).
REX_EXTERN(__imp__sub_822F2688);
REX_HOOK_RAW(sub_822F2688) {
  __imp__sub_822F2688(ctx, base);
  if (Enabled()) {
    std::lock_guard lock(g_mutex);
    g_submits++;
    g_lastSubmit = std::chrono::steady_clock::now();
  }
}
REX_EXTERN(__imp__sub_822F2870);
REX_HOOK_RAW(sub_822F2870) {
  using ng2::guest::Load32;
  if (!Enabled()) {
    __imp__sub_822F2870(ctx, base);
    return;
  }
  uint32_t worker = ctx.r3.u32;
  uint32_t before[6] = {Load32(worker), Load32(worker + 4), Load32(0x83C07CF0 + 8),
                        Load32(0x83C07CF0 + 12), Load32(0x83C07CF0 + 36), Load32(0x83C07CF0 + 24)};
  {
    std::lock_guard lock(g_mutex);
    g_runs++;
    g_lastRun = std::chrono::steady_clock::now();
    g_lastRunner = worker;
  }
  __imp__sub_822F2870(ctx, base);
  {
    std::lock_guard lock(g_mutex);
    g_runLog[g_runLogNext++ % 8] = fmt::format(
        "worker {:#x} id {} slot {} pop {} count {} mask {:#x} done {} -> pop {} done {}", worker,
        before[0], before[1], before[2], before[3], before[4], before[5],
        Load32(0x83C07CF0 + 8), Load32(0x83C07CF0 + 24));
  }
}
