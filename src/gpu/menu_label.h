// ng2 - Ninja Gaiden II static recompilation
//
// The RECOMP SETTINGS and EXIT GAME title menu labels (menu_label.cpp).

#pragma once

#include <cstdint>

namespace ng2::video {

enum class LabelResult { kUnchanged, kRewritten, kRetry };

// Called with each decoded 8-bit texture's base level: keeps the title
// menu's letter atlas and rewrites its EXIT DEMO entry, and rewrites the
// Marketplace label (kRetry: seen before the atlas, upload it again later).
// rows: how many rows from the top were rewritten, for the mips.
LabelResult RewriteMenuLabel(uint32_t width, uint32_t height, uint8_t* pixels, uint32_t* rows);

}  // namespace ng2::video
