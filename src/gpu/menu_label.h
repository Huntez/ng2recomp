// ng2 - Ninja Gaiden II static recompilation
//
// The RECOMP SETTINGS title menu label (menu_label.cpp).

#pragma once

#include <cstdint>

namespace ng2::video {

enum class LabelResult { kUnchanged, kRewritten, kRetry };

// Called with each decoded 8-bit texture: keeps the title menu's letter
// atlas, and rewrites the Marketplace label (kRetry: seen before the atlas,
// upload it again later).
LabelResult RewriteMenuLabel(uint32_t width, uint32_t height, uint8_t* pixels);

}  // namespace ng2::video
