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
// Both back off: a few pause instructions, then yields, then short sleeps (yields on
// Windows, whose sleeps are whole milliseconds).

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include <rex/hook.h>

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

// Before the compare: r11 is the handoff word, 0 once the frame is taken.
void Ng2FrameHandoffWait(PPCRegister& r11) {
  if (r11.u32 != 0) {
    Backoff();
  }
}

void Ng2VblankWait() {
  Backoff();
}
