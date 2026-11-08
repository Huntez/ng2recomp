// ng2 - Ninja Gaiden II static recompilation
//
// Two of the game's busy-waits, which on the 360 had hardware threads to
// themselves and here hold whole cores: on a 4-core handheld they take CPU
// time from the threads they wait for.
//
// The main loop (sub_822F34C0) hands each frame to the render thread, then
// spins until the render thread has taken it (lwz r11 of 0x84C53430 until
// it reads 0): nearly all of the main thread's time on a fast PC.
//
// The render thread's frame limiter (sub_822F0BF8) spins until the vblank
// count (0x84C55B50, from the vblank interrupt) is the word at 0x84C5342C
// past where it started, with the PowerPC low-priority hint in the loop.
//
// When the game asks for fewer frames than vblanks (the word at 0x84C5342C:
// 2 on the game over screen), the limiter reads the count (sub_822F02D8)
// once to start, then until it has moved on by that many. On the console the
// frame's work before it fits in a vblank, so it starts in the vblank the
// last wait ended in and every frame takes exactly 2. Here Present can wait
// on the host display's vsync, which isn't in step with the guest's vblanks,
// and push the start a vblank or two late: that frame took 3 or 4 (27 fps,
// not 30, uneven). A start up to the interval past the last wait's end
// counts from that end, so a frame takes the interval unless its own work
// took longer than that (where the console would also run late).
//
// Both back off: a few pause instructions, then yields, then short sleeps (yields on
// Windows, whose sleeps are whole milliseconds).

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <thread>

#include <rex/logging.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include <rex/hook.h>

#include "gpu/guest.h"
#include "gpu/video.h"

namespace {

// One step of a spin; a new wait (a gap since the last step) starts over.
void Backoff() {
  using clock = std::chrono::steady_clock;
  thread_local uint32_t spins = 0;
  thread_local clock::time_point last;
  auto now = clock::now();
  if (now - last > std::chrono::microseconds(500)) {
    spins = 0;
  }
  spins++;
  if (spins < 64) {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
  } else if (spins < 256) {
    std::this_thread::yield();
  } else {
#if defined(_WIN32)
    // MSVC's sleep_for rounds up to whole milliseconds (Sleep(1), 1-2 ms): too coarse here.
    std::this_thread::yield();
#else
    std::this_thread::sleep_for(std::chrono::microseconds(50));
#endif
  }
  last = clock::now();
}

}  // namespace

// NG2_LATE_INPUT=1: the pad read later, as Reflex does. The main thread
// reads the pad as soon as the render thread has taken the last frame, works
// the next out in a few ms, then waits (most of a frame on a fast PC) for the
// render thread to take it at its next vblank. Waiting before the read
// instead, by that wait less a margin, the frame shows the same time but with
// a fresher pad. The wait is the least of the last 30 frames' less 3 ms; a
// frame that came in under 1 ms of being late takes 4 ms off at once.
namespace {
using SteadyClock = std::chrono::steady_clock;
int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch()).count();
}
const bool kLateInput = [] {
  const char* v = std::getenv("NG2_LATE_INPUT");
  return v && v[0] == '1';
}();
int64_t g_takenNs = 0;        // the render thread took the last frame
int64_t g_handoffNs = 0;      // the main thread began waiting for it to (0: not yet this frame)
int64_t g_lateDelayNs = 0;    // how long after the take the pad is read
int64_t g_windowMinNs = INT64_MAX;
uint32_t g_windowFrames = 0;
// (log, each 10 s)
int64_t g_logAtNs = 0, g_delaySum = 0, g_spinSum = 0;
uint32_t g_logFrames = 0, g_lateFrames = 0;

void NoteHandoffTaken(bool waited) {
  int64_t now = NowNs();
  int64_t spin = waited && g_handoffNs ? now - g_handoffNs : 0;
  g_takenNs = now;
  g_handoffNs = 0;
  if (!kLateInput) {
    return;
  }
  constexpr int64_t kMargin = 3'000'000, kMax = 12'000'000;
  if (spin < 1'000'000) {
    g_lateDelayNs = std::max<int64_t>(0, g_lateDelayNs - 4'000'000);
    g_windowMinNs = INT64_MAX, g_windowFrames = 0;
    g_lateFrames++;
  } else {
    g_windowMinNs = std::min(g_windowMinNs, spin);
    if (++g_windowFrames >= 30) {
      g_lateDelayNs = std::clamp<int64_t>(g_lateDelayNs + (g_windowMinNs - kMargin) / 2, 0, kMax);
      g_windowMinNs = INT64_MAX, g_windowFrames = 0;
    }
  }
  g_delaySum += g_lateDelayNs, g_spinSum += spin, g_logFrames++;
  if (!g_logAtNs) {
    g_logAtNs = now;
  }
  if (now - g_logAtNs > 10'000'000'000 && g_logFrames) {
    REXLOG_INFO("late input: the pad read {:.1f} ms later, the main thread then waiting {:.1f} ms "
                "for the take; {} of {} frames near late",
                double(g_delaySum) / g_logFrames / 1e6, double(g_spinSum) / g_logFrames / 1e6, g_lateFrames,
                g_logFrames);
    g_logAtNs = now, g_delaySum = g_spinSum = 0, g_logFrames = g_lateFrames = 0;
  }
}
}  // namespace

// The input update (gameplay.cpp), before it reads the pads.
void Ng2BeforeInputRead() {
  if (!kLateInput || !g_lateDelayNs || !g_takenNs) {
    return;
  }
  int64_t target = g_takenNs + g_lateDelayNs;
  int64_t now = NowNs();
  if (now >= target || target - now > 16'000'000) {
    return;
  }
  while (target - now > 2'000'000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    now = NowNs();
  }
  while (NowNs() < target) {
    std::this_thread::yield();
  }
}

// Before the compare: r11 is the handoff word, 0 once the frame is taken.
void Ng2FrameHandoffWait(PPCRegister& r11) {
  if (r11.u32 != 0) {
    if (!g_handoffNs) {
      g_handoffNs = NowNs();
    }
    Backoff();
  } else {
    NoteHandoffTaken(g_handoffNs != 0);
    ng2::video::NoteFrameTaken();  // (NG2_LATENCY_LOG)
  }
}

void Ng2VblankWait() {
  Backoff();
}

// The limiter's reads of the vblank count: its start (returning to 822F0C40)
// and each check in its loop (to 822F0C4C).
REX_EXTERN(__imp__sub_822F02D8);
REX_HOOK_RAW(sub_822F02D8) {
  constexpr uint32_t kStart = 0x822F0C40, kCheck = 0x822F0C4C, kInterval = 0x84C5342C;
  static uint32_t lastEnd = 0;
  static bool waited = false;
  uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_822F02D8(ctx, base);
  if (lr == kStart) {
    // Up to the interval since the last wait ended: blocked, not slow.
    uint32_t interval = ng2::guest::Load32(kInterval);
    if (waited && ctx.r3.u32 - lastEnd <= interval) {
      ctx.r3.u32 = lastEnd;
    }
  } else if (lr == kCheck) {
    lastEnd = ctx.r3.u32;  // the last check is the one the wait ended on
    waited = true;
  }
}
