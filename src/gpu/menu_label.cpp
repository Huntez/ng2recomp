// ng2 - Ninja Gaiden II static recompilation
//
// The title menu's "Xbox LIVE Marketplace" picture becomes "RECOMP SETTINGS"
// (src/recomp_settings.cpp), made from the game's own letters: the other
// entries' pictures share one 8-bit atlas, one 90-pixel band per entry, in
// the same font. Each word there is split into its letters at the columns
// with the fewest bright pixels, and the new text is laid out from those
// columns (letter, outline and glow), so it takes the menu's palettes like
// the rest. Nothing of the game's art is stored here.

#include "menu_label.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string_view>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace ng2::video {
namespace {

constexpr uint32_t kWidth = 520;
constexpr uint32_t kBand = 90;
constexpr uint64_t kAtlasHash = 0x3737e258b350fa36;   // 520x990, the entries' atlas
constexpr uint64_t kLabelHash = 0xf8fb387ec4883ffc;   // 520x90, Xbox LIVE Marketplace
constexpr std::string_view kText = "RECOMP SETTINGS";
// The atlas's entries, top to bottom.
constexpr std::string_view kWords[] = {
    "EXIT DEMO", "NEW GAME", "LOAD GAME", "LEADERBOARDS", "NINJA CINEMA", "OPTIONS", "MISSIONS",
    "PATH OF THE ACOLYTE", "PATH OF THE WARRIOR", "PATH OF THE MENTOR", "PATH OF THE MASTER NINJA"};
constexpr uint8_t kClear = 255;  // outside the picture
constexpr uint8_t kPlate = 0;    // the dark plate behind the words
constexpr uint32_t kSpace = 16;  // columns between words

std::vector<uint8_t> g_atlas;  // the atlas's pixels, once seen

struct Glyph {
  uint32_t band, begin, end;  // columns
};

bool IsLetter(uint8_t v) {
  return v != kClear && v != kPlate && (v <= 70 || v >= 110);
}

// The columns of each letter of the atlas's words.
std::map<char, Glyph> FindGlyphs() {
  std::map<char, Glyph> glyphs;
  for (uint32_t band = 0; band < std::size(kWords); band++) {
    const uint8_t* pixels = g_atlas.data() + band * kBand * kWidth;
    // Words: runs of columns holding letter pixels.
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    bool inside = false;
    for (uint32_t x = 0; x <= kWidth; x++) {
      bool ink = false;
      for (uint32_t y = 0; x < kWidth && y < kBand && !ink; y++) {
        ink = IsLetter(pixels[y * kWidth + x]);
      }
      if (ink && !inside) {
        runs.emplace_back(x, x);
      } else if (!ink && inside) {
        runs.back().second = x;
      }
      inside = ink;
    }
    std::vector<std::string_view> parts;
    for (std::string_view words = kWords[band]; !words.empty();) {
      size_t space = words.find(' ');
      parts.push_back(words.substr(0, space));
      words = space == std::string_view::npos ? std::string_view() : words.substr(space + 1);
    }
    if (runs.size() != parts.size()) {
      continue;
    }
    for (size_t w = 0; w < parts.size(); w++) {
      auto [begin, end] = runs[w];
      size_t letters = parts[w].size();
      // Bright pixels per column, smoothed; the letters' joins are its
      // lowest points.
      std::vector<float> profile(end - begin);
      for (uint32_t x = begin; x < end; x++) {
        uint32_t count = 0;
        for (uint32_t y = 0; y < kBand; y++) {
          uint8_t v = pixels[y * kWidth + x];
          count += v >= 150 && v != kClear;
        }
        profile[x - begin] = float(count);
      }
      std::vector<float> smooth(profile.size());
      for (size_t i = 0; i < profile.size(); i++) {
        float left = i ? profile[i - 1] : 0.0f;
        float right = i + 1 < profile.size() ? profile[i + 1] : 0.0f;
        smooth[i] = left + 2.0f * profile[i] + right;
      }
      std::vector<uint32_t> order(smooth.size());
      for (uint32_t i = 0; i < order.size(); i++) {
        order[i] = i;
      }
      std::stable_sort(order.begin(), order.end(),
                       [&](uint32_t a, uint32_t b) { return smooth[a] < smooth[b]; });
      std::vector<uint32_t> cuts;
      for (uint32_t c : order) {
        if (cuts.size() + 1 >= letters) {
          break;
        }
        if (c < 6 || c + 6 > smooth.size()) {
          continue;
        }
        if (std::all_of(cuts.begin(), cuts.end(),
                        [&](uint32_t x) { return (c > x ? c - x : x - c) >= 10; })) {
          cuts.push_back(c);
        }
      }
      if (cuts.size() + 1 != letters) {
        continue;
      }
      std::sort(cuts.begin(), cuts.end());
      uint32_t from = begin;
      for (size_t i = 0; i < letters; i++) {
        uint32_t to = i < cuts.size() ? begin + cuts[i] : end;
        glyphs.try_emplace(parts[w][i], Glyph{band, from, to});
        from = to;
      }
    }
  }
  return glyphs;
}

// The new label into pixels (kWidth x kBand), false if a letter is missing.
bool Compose(uint8_t* pixels) {
  auto glyphs = FindGlyphs();
  uint32_t width = 0;
  for (char c : kText) {
    if (c == ' ') {
      width += kSpace;
    } else if (auto it = glyphs.find(c); it != glyphs.end()) {
      width += it->second.end - it->second.begin;
    } else {
      return false;
    }
  }
  if (width > kWidth) {
    return false;
  }
  // The plate's rows, from the NEW GAME entry.
  std::array<bool, kBand> plateRows{};
  for (uint32_t y = 0; y < kBand; y++) {
    const uint8_t* row = g_atlas.data() + (kBand + y) * kWidth;
    plateRows[y] = std::find(row, row + kWidth, kPlate) != row + kWidth;
  }
  std::memset(pixels, kClear, kWidth * kBand);
  uint32_t x = (kWidth - width) / 2;
  for (char c : kText) {
    if (c == ' ') {
      for (uint32_t i = 0; i < kSpace; i++, x++) {
        for (uint32_t y = 0; y < kBand; y++) {
          pixels[y * kWidth + x] = plateRows[y] ? kPlate : kClear;
        }
      }
      continue;
    }
    const Glyph& glyph = glyphs[c];
    for (uint32_t gx = glyph.begin; gx < glyph.end; gx++, x++) {
      for (uint32_t y = 0; y < kBand; y++) {
        pixels[y * kWidth + x] = g_atlas[(glyph.band * kBand + y) * kWidth + gx];
      }
    }
  }
  return true;
}

}  // namespace

LabelResult RewriteMenuLabel(uint32_t width, uint32_t height, uint8_t* pixels) {
  if (width != kWidth || (height != kBand && height != kBand * std::size(kWords))) {
    return LabelResult::kUnchanged;
  }
  uint64_t hash = XXH3_64bits(pixels, size_t(width) * height);
  if (hash == kAtlasHash) {
    g_atlas.assign(pixels, pixels + size_t(width) * height);
    return LabelResult::kUnchanged;
  }
  if (hash != kLabelHash) {
    return LabelResult::kUnchanged;
  }
  if (g_atlas.empty()) {
    return LabelResult::kRetry;
  }
  return Compose(pixels) ? LabelResult::kRewritten : LabelResult::kUnchanged;
}

}  // namespace ng2::video
