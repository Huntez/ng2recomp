// ng2 - Ninja Gaiden II static recompilation
//
// The RECOMP SETTINGS and EXIT GAME title menu labels (menu_label.cpp).

#pragma once

#include <cstdint>
#include <vector>

namespace ng2::video {

enum class LabelResult { kUnchanged, kRewritten, kRetry };

// Called with each decoded 8-bit texture's base level: keeps the title
// menu's letter atlas and rewrites its EXIT DEMO entry, and rewrites the
// Marketplace label (kRetry: seen before the atlas, upload it again later).
// rows: how many rows from the top were rewritten, for the mips.
LabelResult RewriteMenuLabel(uint32_t width, uint32_t height, uint8_t* pixels, uint32_t* rows);

// The Options screen's "OPTIONS" header (a 172x58 8-bit picture) reads
// "RECOMP SETTINGS" while ours is open: a wider picture made from the
// atlas's letters in the header's look. ReplaceMenuHeader gives it for the
// header's base level (false: keep the game's); MenuHeaderGeneration changes
// whenever the wanted picture does, so a cached one is made again. The
// in-game list's header (521x58, same lettering) reads the same while ours is
// shown there, at its own width.
void SetRecompHeader(bool on);
void SetInGameRecompHeader(bool on);
uint32_t MenuHeaderGeneration();
// (kind: which header the picture is, ours or not.)
enum class MenuHeaderKind { kNone, kOptions, kInGame };
bool ReplaceMenuHeader(uint32_t width, uint32_t height, const uint8_t* pixels,
                       std::vector<uint8_t>& out, uint32_t& outWidth, MenuHeaderKind& kind);

}  // namespace ng2::video
