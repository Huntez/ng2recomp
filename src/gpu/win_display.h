// ng2 - Ninja Gaiden II static recompilation
//
// Windows timing helpers: a precise sleep, and the monitor's own vblanks
// (for GPUs whose Vulkan driver lacks VK_KHR_present_wait). Elsewhere the
// sleep is std::this_thread::sleep_for and the display clock never starts.

#pragma once

#include <atomic>
#include <cstdint>

namespace ng2::video {

// Sleeps about ns. Windows: a high-resolution waitable timer (Sleep and
// sleep_for wake on the scheduler's tick, up to a millisecond or more late).
void PreciseSleepNs(int64_t ns);
// Whether that sleep is precise on this thread (Windows before 10 1803: no).
bool PreciseSleepAvailable();

// What a display clock reports after each of the monitor's vblanks: its time
// (now()'s clock), the refresh period, and whether that period can be locked to.
using DisplayVblankReport = void (*)(int64_t vblankNs, int64_t periodNs, bool steady);

// Windows: starts a thread that waits for each vblank of the monitor the
// window is on (DXGI IDXGIOutput::WaitForVBlank) and reports it while run is
// set. False where there is no such clock (not Windows, or no window).
bool StartWindowsDisplayClock(void* hwnd, const std::atomic<bool>* run, int64_t (*now)(),
                              DisplayVblankReport report);

}  // namespace ng2::video
