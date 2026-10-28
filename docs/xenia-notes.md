# Notes from Xenia (Edge)

Xenia Edge (https://github.com/has207/xenia-edge, BSD-3-Clause; a fork of
Xenia Canary) is a reference for GPU and kernel behaviour alongside the
Xenia-derived code in ReXGlue. Its compatibility list entry for NG2 (title
544307D5, issue 296): state "Gameplay", labels gpu-drawing-missing,
gpu-drawing-corrupt, kernel-save-file-errors, vsync-off-speedup,
requires_protect_zero_false, gpu-driver-issues-nvidia,
requires_clear_memory_page_state_true.

What NG2 does with that:

- protect_zero: NG2 sets it to false by default (src/ng2_app.h); the game
  reads the zero page.
- clear_memory_page_state: already true by default in ReXGlue (it only
  affects the xenos plugin).
- Xenia writes 0x2A6E3F38 at 0x8000001C because NG2 reads it; ReXGlue does
  the same (src/system/xmemory.cpp).
- Save file errors: to check when saving is reached.
