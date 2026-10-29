// ng2 - Ninja Gaiden II static recompilation
//
// RECOMP SETTINGS: the title menu's Xbox LIVE Marketplace entry opens the
// game's own Options screen with the recomp's settings in it
// (docs/menus.md). While it is open:
//
// - The Camera Settings page shows the video settings: its row and choice
//   texts are rewritten in the loaded string table (in place, each within its
//   16-byte-aligned slot) and its four values, bytes of the game's settings
//   object, hold ours around each run of the page's handler, so the game's
//   own input, sounds and drawing edit them. The player's camera settings
//   are put back after every run.
// - The two sliders get new ranges (Bloom 0-2, V-Sync 0-1) in the game's
//   option range table.
//
// Everything is restored and the settings are saved when Options closes.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "gpu/guest.h"
#include "recomp_settings.h"

REXCVAR_DECLARE(int32_t, render_scale);
REXCVAR_DECLARE(int32_t, anisotropic_filtering);
REXCVAR_DECLARE(int32_t, bloom);
REXCVAR_DECLARE(bool, vsync);

namespace ng2::recomp_settings {
namespace {

// The title menu opens screens with sub_8364CFC8(manager, id): 13 is the
// "sign in to see the Marketplace" message, 9 the Options screen.
constexpr uint32_t kMarketplaceMessage = 13;
constexpr uint32_t kOptionsScreen = 9;

// The game's settings object, and the Camera page's values in it
// (sub_823E7040): 1st/3rd-person camera controls, 1st/3rd-person speeds.
constexpr uint32_t kSettingsPointer = 0x84C3B9E4;
constexpr uint32_t kValueOffsets[4] = {0x3B, 0x3C, 0x3E, 0x3F};

// Option ranges: 12-byte entries (min, max, default) from 0x83ADC2D8; the
// Camera page's sliders use entries 0x1A and 0x1B.
constexpr uint32_t kRangeTable = 0x83ADC2D8;
constexpr uint32_t kSliderEntries[2] = {0x1A, 0x1B};
constexpr int32_t kSliderRanges[2][3] = {{0, 2, 2}, {0, 1, 1}};  // Bloom, V-Sync

// Texts, by index in the Options string table (table 2: "Game Settings"
// first, strings back to back in 16-byte slots).
struct Text {
  uint32_t index;
  std::u16string_view text;
};
constexpr Text kTexts[] = {
    {0x97, u"Graphics"},           // Camera Settings
    {0x06, u"Resolution"},         // 1st-Person Camera Controls
    {0x07, u"Texture Filtering"},  // 3rd-Person Camera Controls
    {0x14, u"Low"},                // Default
    {0x15, u"Medium"},             // Vertical Inverted
    {0x16, u"High"},               // Horizontal Inverted
    {0x17, u"Ultra"},              // All Inverted
    {0xAA, u"Bloom"},              // 1st-Person Cam. Speed
    {0xAB, u"V-Sync"},             // 3rd-Person Cam. Speed
};

bool g_open = false;
bool g_changed = false;  // a setting changed while open: save on closing
// The settings the menu edits, as saved in the config file.
constexpr const char* kSettingNames[] = {"render_scale", "anisotropic_filtering", "bloom",
                                         "vsync"};
std::filesystem::path g_configPath;
struct SavedBytes {
  uint32_t physical;
  std::vector<uint8_t> bytes;  // the game's
  std::vector<uint8_t> ours;   // what we wrote, to check before restoring
};
std::vector<SavedBytes> g_savedTexts;
int32_t g_savedRanges[2][3];

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void StoreBE32(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value >> 24), p[1] = uint8_t(value >> 16), p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

std::vector<uint8_t> Utf16BE(std::u16string_view text) {
  std::vector<uint8_t> bytes;
  for (char16_t c : text) {
    bytes.push_back(uint8_t(c >> 8));
    bytes.push_back(uint8_t(c));
  }
  return bytes;
}

// The physical address of each string slot, from the table found by its
// first two strings; empty if the table isn't loaded. The table's address
// is kept and checked first: a scan reads all of guest memory.
std::vector<std::pair<uint32_t, uint32_t>> FindTextSlots() {
  constexpr uint32_t kPhysicalSize = 512u << 20;
  constexpr uint32_t kSlotCount = 0xAC;  // past the last index used
  static uint32_t knownTable = ~0u;
  const uint8_t* memory = guest::Physical(0);
  std::vector<uint8_t> first = Utf16BE(u"Game Settings");
  first.push_back(0), first.push_back(0);
  std::vector<uint8_t> second = Utf16BE(u"Controller Settings");
  const uint8_t* end = memory + kPhysicalSize;
  auto isTable = [&](const uint8_t* hit) {
    return uint32_t(hit - memory) % 16 == 0 && hit + 0x20 + second.size() < end &&
           std::memcmp(hit, first.data(), first.size()) == 0 &&
           std::memcmp(hit + 0x20, second.data(), second.size()) == 0;
  };
  const uint8_t* table = nullptr;
  if (knownTable != ~0u && isTable(memory + knownTable)) {
    table = memory + knownTable;
  }
  for (const uint8_t* at = memory; !table && at < end;) {
    const uint8_t* hit = static_cast<const uint8_t*>(
        memmem(at, size_t(end - at), first.data(), first.size()));
    if (!hit) {
      break;
    }
    if (isTable(hit)) {
      table = hit;
    }
    at = hit + 2;
  }
  if (!table) {
    return {};
  }
  knownTable = uint32_t(table - memory);
  std::vector<std::pair<uint32_t, uint32_t>> slots;  // address, size
  uint32_t address = knownTable;
  for (uint32_t i = 0; i < kSlotCount && address + 2 <= kPhysicalSize; i++) {
    uint32_t length = 0;
    while (address + length + 1 < kPhysicalSize &&
           (memory[address + length] | memory[address + length + 1])) {
      length += 2;
    }
    uint32_t size = (length + 2 + 15) & ~15u;
    slots.emplace_back(address, size);
    address += size;
  }
  return slots;
}

void Open() {
  if (g_open) {
    return;
  }
  g_open = true;
  auto slots = FindTextSlots();
  if (slots.empty()) {
    REXLOG_WARN("recomp settings: Options texts not found");
  }
  for (const Text& text : kTexts) {
    if (text.index >= slots.size()) {
      continue;
    }
    auto [address, size] = slots[text.index];
    std::vector<uint8_t> bytes = Utf16BE(text.text);
    if (bytes.size() + 2 > size) {
      continue;  // doesn't fit the slot
    }
    uint8_t* slot = guest::Physical(address);
    SavedBytes saved{address, std::vector<uint8_t>(slot, slot + size), {}};
    std::memset(slot, 0, size);
    std::memcpy(slot, bytes.data(), bytes.size());
    saved.ours.assign(slot, slot + size);
    g_savedTexts.push_back(std::move(saved));
  }
  for (int i = 0; i < 2; i++) {
    uint8_t* entry = guest::Virtual(kRangeTable + kSliderEntries[i] * 12);
    for (int j = 0; j < 3; j++) {
      g_savedRanges[i][j] = int32_t(LoadBE32(entry + j * 4));
      StoreBE32(entry + j * 4, uint32_t(kSliderRanges[i][j]));
    }
  }
  REXLOG_INFO("recomp settings: open");
}

// A "name = value" line (leading spaces already gone).
bool IsKeyLine(std::string_view line, std::string_view name) {
  if (line.substr(0, name.size()) != name) {
    return false;
  }
  line.remove_prefix(name.size());
  size_t next = line.find_first_not_of(" \t");
  return next != std::string_view::npos && line[next] == '=';
}

// Writes the menu's settings into the config file, keeping the rest of it:
// each replaces its own top-level line, or is added before the first table.
void SaveSettings() {
  std::vector<std::string> lines;
  if (std::ifstream in(g_configPath); in) {
    for (std::string line; std::getline(in, line);) {
      lines.push_back(line);
    }
  }
  for (const char* name : kSettingNames) {
    std::string entry = std::string(name) + " = " + rex::cvar::GetFlagByName(name);
    size_t firstTable = lines.size();
    bool replaced = false;
    for (size_t i = 0; i < lines.size(); i++) {
      std::string_view line = lines[i];
      line.remove_prefix(std::min(line.find_first_not_of(" \t"), line.size()));
      if (!line.empty() && line[0] == '[') {
        firstTable = std::min(firstTable, i);
        break;
      }
      if (IsKeyLine(line, name)) {
        lines[i] = entry;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      lines.insert(lines.begin() + std::ptrdiff_t(firstTable), entry);
    }
  }
  std::ofstream out(g_configPath, std::ios::trunc);
  for (const std::string& line : lines) {
    out << line << '\n';
  }
  REXLOG_INFO("recomp settings: saved to {}", g_configPath.string());
}

void Close() {
  if (!g_open) {
    return;
  }
  g_open = false;
  for (const SavedBytes& saved : g_savedTexts) {
    uint8_t* slot = guest::Physical(saved.physical);
    if (std::memcmp(slot, saved.ours.data(), saved.ours.size()) == 0) {
      std::memcpy(slot, saved.bytes.data(), saved.bytes.size());
    }
  }
  g_savedTexts.clear();
  for (int i = 0; i < 2; i++) {
    uint8_t* entry = guest::Virtual(kRangeTable + kSliderEntries[i] * 12);
    for (int j = 0; j < 3; j++) {
      StoreBE32(entry + j * 4, uint32_t(g_savedRanges[i][j]));
    }
  }
  if (g_changed && !g_configPath.empty()) {
    SaveSettings();
  }
  g_changed = false;
  REXLOG_INFO("recomp settings: closed");
}

// The settings as the Camera page's four values, and back.
std::array<uint8_t, 4> ToValues() {
  int32_t filtering = REXCVAR_GET(anisotropic_filtering);
  uint8_t filter = filtering >= 5 ? 3 : filtering == 4 ? 2 : filtering == 3 ? 1 : 0;
  return {uint8_t(std::clamp(REXCVAR_GET(render_scale), 1, 4) - 1), filter,
          uint8_t(std::clamp(REXCVAR_GET(bloom), 0, 2)), uint8_t(REXCVAR_GET(vsync) ? 1 : 0)};
}

// Only the rows the page changed: a setting the four choices can't show
// exactly (filtering off, 1x, 2x) stays as it is until its row is changed.
void FromValues(const std::array<uint8_t, 4>& before, const std::array<uint8_t, 4>& values) {
  static constexpr int32_t kFiltering[4] = {-1, 3, 4, 5};  // game, 4x, 8x, 16x
  std::string text[4] = {std::to_string(std::min<int>(values[0], 3) + 1),
                         std::to_string(kFiltering[std::min<int>(values[1], 3)]),
                         std::to_string(std::min<int>(values[2], 2)),
                         values[3] ? "true" : "false"};
  for (int i = 0; i < 4; i++) {
    if (values[i] != before[i] && rex::cvar::GetFlagByName(kSettingNames[i]) != text[i]) {
      rex::cvar::SetFlagByName(kSettingNames[i], text[i]);
      g_changed = true;
    }
  }
}

}  // namespace

void SetConfigPath(const std::filesystem::path& path) {
  g_configPath = path;
}

}  // namespace ng2::recomp_settings

using namespace ng2::recomp_settings;

// Opening a screen: the Marketplace message becomes the Options screen with
// our settings in it.
REX_EXTERN(__imp__sub_8364CFC8);
REX_HOOK_RAW(sub_8364CFC8) {
  if (ctx.r4.u32 == kMarketplaceMessage) {
    ctx.r4.u64 = kOptionsScreen;
    Open();
  }
  __imp__sub_8364CFC8(ctx, base);
}

// The Options screen's run: its loop (sub_823E5778) returns when Options
// closes.
REX_EXTERN(__imp__sub_823E55B0);
REX_HOOK_RAW(sub_823E55B0) {
  __imp__sub_823E55B0(ctx, base);
  Close();
}

// The Camera Settings page, run with our values in place of the player's.
REX_EXTERN(__imp__sub_823E7040);
REX_HOOK_RAW(sub_823E7040) {
  if (!g_open) {
    __imp__sub_823E7040(ctx, base);
    return;
  }
  uint32_t settings = ng2::guest::Load32(kSettingsPointer);
  uint8_t* object = ng2::guest::Virtual(settings);
  std::array<uint8_t, 4> player, shown = ToValues(), ours = shown;
  for (int i = 0; i < 4; i++) {
    player[i] = object[kValueOffsets[i]];
    object[kValueOffsets[i]] = ours[i];
  }
  __imp__sub_823E7040(ctx, base);
  for (int i = 0; i < 4; i++) {
    ours[i] = object[kValueOffsets[i]];
    object[kValueOffsets[i]] = player[i];
  }
  FromValues(shown, ours);
}
