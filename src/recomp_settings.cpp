// ng2 - Ninja Gaiden II static recompilation
//
// RECOMP SETTINGS: the title menu's Xbox LIVE Marketplace entry opens the
// game's own Options screen as a screen of the recomp's settings
// (docs/menus.md). While it is open:
//
// - Options starts on the Game Settings page, and Back leaves Options; the
//   category list shows only "Recomp Settings" (the other names are blanked
//   in the loaded string table).
// - The page's builder (sub_836AC710) runs as usual, but its rows and choices
//   are ours: the row and choice calls it makes are rewritten, rows we don't
//   use are left out and the rows below them moved up. Our texts go in
//   spare slots of the string table (the Music Test's track names).
// - The page's handler (sub_823E6490) is replaced by one that edits our
//   settings, using the game's own input, sound and menu-model functions.
//
// Everything is restored and the settings are saved when Options closes.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app_context.h>

#include "gpu/guest.h"
#include "recomp_settings.h"

namespace ng2::recomp_settings {
namespace {

// The title menu opens screens with sub_8364CFC8(manager, id): 13 is the
// "sign in to see the Marketplace" message, 9 the Options screen.
constexpr uint32_t kMarketplaceMessage = 13;
constexpr uint32_t kOptionsScreen = 9;

// Options state (sub_823E5778): the page (0 the category list, 1 Game
// Settings, past 11 closes Options), the page before this frame's, the
// depth, and a cursor per page.
constexpr uint32_t kPage = 0x83CEB3F8;
constexpr uint32_t kLastPage = 0x83CEB3FC;
constexpr uint32_t kDepth = 0x83CEB400;
constexpr uint32_t kCursors = 0x83CEB414;
constexpr uint32_t kGamePage = 1;
constexpr uint32_t kClosed = 0xFF;  // what the category list's Back sets
// The pad state, the player's pad, and the menu model.
constexpr uint32_t kPadState = 0x84F41264;
constexpr uint32_t kPlayerPad = 0x84B5030A;
constexpr uint32_t kMenuModel = 0x85313E30;
constexpr uint32_t kCursorSound = 0x8206EE58;  // the float the sounds take
// sub_83682750(n): the menu's transitions.
constexpr uint32_t kChoiceChanged = 5;
constexpr uint32_t kEnter = 6;
constexpr uint32_t kLeave = 7;

// The Game Settings page object's rows, in the order its builder makes them:
// the language list, six choice rows, the play time, Restore Defaults.
constexpr uint32_t kLanguageRow = 0x1E0;
constexpr uint32_t kChoiceRows[6] = {0x450, 0xA60, 0x1070, 0x1680, 0x1C90, 0x22A0};
constexpr uint32_t kPlayTime = 0x28B0;
constexpr uint32_t kRestoreRow = 0x2AD0;
constexpr uint32_t kPlayTimeText = 0x859D8DC4;  // UTF-16, from sub_836AC0C8
constexpr uint32_t kRowY = 0x1C;  // a row's position: x at +0x18, y here
constexpr uint32_t kOnOffRow = 0x600;  // a choice row's flag: 1 shows its first choice

// Options strings: table 2 (id 0x20000 + index), strings back to back in
// 16-byte slots.
constexpr uint32_t kTable = 0x20000;
constexpr uint32_t kCategoryName = 0x00;  // Game Settings
// The other categories' names, blanked.
constexpr uint32_t kHiddenNames[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x61, 0x97};
constexpr uint32_t kPlayTimeName = 0x2F;
// The Music Test's track names: room for our texts.
constexpr uint32_t kSpareFirst = 0x63, kSpareEnd = 0x97;
constexpr uint32_t kSlotCount = 0x98;  // past the last index used

struct Row {
  std::u16string_view label;
  std::array<std::u16string_view, 4> choices;
  uint32_t count;
  const char* setting;
};
constexpr Row kRows[] = {
    {u"Render Scale", {u"1x", u"2x", u"3x", u"4x"}, 4, "render_scale"},
    {u"Texture Filtering", {u"Default", u"4x", u"8x", u"16x"}, 4, "anisotropic_filtering"},
    {u"Bloom", {u"Off", u"Reduced", u"On"}, 3, "bloom"},
    {u"V-Sync", {u"On", u"Off"}, 2, "vsync"},
    {u"Fullscreen", {u"On", u"Off"}, 2, "fullscreen"},
};
constexpr uint32_t kSettingRows = std::size(kRows);
constexpr uint32_t kRowCount = kSettingRows + 1;  // and Restore Defaults
constexpr std::u16string_view kRestoreText = u"Restore Defaults";
constexpr std::u16string_view kTitle = u"Recomp Settings";

bool g_open = false;
bool g_entered = false;  // the category list has opened our page
// The settings changed while open, as set (fullscreen is applied later, on
// the UI thread): saved on closing.
std::map<std::string, std::string, std::less<>> g_written;
// The game's own Game Settings values in the menu model, which ours replace.
uint32_t g_modelSlots = 0;
std::vector<uint8_t> g_savedModel;
std::filesystem::path g_configPath;
rex::ui::WindowedAppContext* g_appContext = nullptr;
struct SavedBytes {
  uint32_t physical;
  std::vector<uint8_t> bytes;  // the game's
  std::vector<uint8_t> ours;   // what we wrote, to check before restoring
};
std::vector<SavedBytes> g_savedTexts;
std::vector<std::pair<std::u16string_view, uint32_t>> g_textIds;  // our texts' string ids

// The page being built, and how far its rows move up for the rows left out.
uint32_t g_building = 0;
struct Placed {
  uint32_t offset;
  float y;  // where the builder put it
};
std::vector<Placed> g_placed;
float g_shift = 0.0f;
bool g_lastSkipped = false;
float g_lastY = 0.0f;

uint32_t LoadBE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void StoreBE32(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value >> 24), p[1] = uint8_t(value >> 16), p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}
void Store32(uint32_t address, uint32_t value) {
  StoreBE32(guest::Virtual(address), value);
}
float LoadFloat(uint32_t address) {
  return guest::LoadFloat(address);
}
void StoreFloat(uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Store32(address, bits);
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

// Writes text into a slot, keeping the game's bytes; false if it doesn't fit.
bool WriteText(const std::pair<uint32_t, uint32_t>& slot, std::u16string_view text) {
  auto [address, size] = slot;
  std::vector<uint8_t> bytes = Utf16BE(text);
  if (bytes.size() + 2 > size) {
    return false;
  }
  uint8_t* at = guest::Physical(address);
  SavedBytes saved{address, std::vector<uint8_t>(at, at + size), {}};
  std::memset(at, 0, size);
  std::memcpy(at, bytes.data(), bytes.size());
  saved.ours.assign(at, at + size);
  g_savedTexts.push_back(std::move(saved));
  return true;
}

// Our texts into the spare slots, longest first, each in the smallest that
// fits.
void PlaceTexts(const std::vector<std::pair<uint32_t, uint32_t>>& slots) {
  std::vector<std::u16string_view> texts{kRestoreText};
  for (const Row& row : kRows) {
    texts.push_back(row.label);
    texts.insert(texts.end(), row.choices.begin(), row.choices.begin() + row.count);
  }
  std::sort(texts.begin(), texts.end(),
            [](auto a, auto b) { return a.size() > b.size() || (a.size() == b.size() && a < b); });
  texts.erase(std::unique(texts.begin(), texts.end()), texts.end());
  std::vector<bool> used(slots.size());
  for (std::u16string_view text : texts) {
    uint32_t best = 0;
    for (uint32_t i = kSpareFirst; i < kSpareEnd && i < slots.size(); i++) {
      if (!used[i] && slots[i].second >= (text.size() + 1) * 2 &&
          (!best || slots[i].second < slots[best].second)) {
        best = i;
      }
    }
    if (best && WriteText(slots[best], text)) {
      used[best] = true;
      g_textIds.emplace_back(text, kTable | best);
    } else {
      REXLOG_WARN("recomp settings: no room for a text");
    }
  }
}

// The string id of one of our texts (a blank one if it wasn't placed).
uint32_t TextId(std::u16string_view text) {
  for (auto [placed, id] : g_textIds) {
    if (placed == text) {
      return id;
    }
  }
  return kTable | 0x34;  // an empty string
}

void Open() {
  if (g_open) {
    return;
  }
  g_open = true;
  auto slots = FindTextSlots();
  if (slots.empty()) {
    REXLOG_WARN("recomp settings: Options texts not found");
  } else if (slots.size() == kSlotCount) {
    // The spare slots first: the table is found by its first name.
    PlaceTexts(slots);
    WriteText(slots[kCategoryName], kTitle);
    for (uint32_t index : kHiddenNames) {
      WriteText(slots[index], u"");
    }
    WriteText(slots[kPlayTimeName], u"");
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
  for (const auto& [name, value] : g_written) {
    std::string entry = name + " = " + value;
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
  // Last written first: a slot written twice gets the game's bytes back.
  for (auto it = g_savedTexts.rbegin(); it != g_savedTexts.rend(); ++it) {
    uint8_t* slot = guest::Physical(it->physical);
    if (std::memcmp(slot, it->ours.data(), it->ours.size()) == 0) {
      std::memcpy(slot, it->bytes.data(), it->bytes.size());
    }
  }
  g_savedTexts.clear();
  g_textIds.clear();
  if (g_modelSlots) {
    std::memcpy(guest::Virtual(g_modelSlots), g_savedModel.data(), g_savedModel.size());
    g_modelSlots = 0;
  }
  if (!g_written.empty() && !g_configPath.empty()) {
    SaveSettings();
  }
  g_written.clear();
  REXLOG_INFO("recomp settings: closed");
}

// The settings as the rows' choices, and back.
constexpr int32_t kFiltering[4] = {-1, 3, 4, 5};  // game, 4x, 8x, 16x

// A setting's value, counting one set but not applied yet.
std::string GetSetting(std::string_view name) {
  if (auto it = g_written.find(name); it != g_written.end()) {
    return it->second;
  }
  return rex::cvar::GetFlagByName(name);
}

int32_t GetInt(std::string_view name) {
  return std::atoi(GetSetting(name).c_str());
}

uint32_t ValueOf(uint32_t row) {
  switch (row) {
    case 0:
      return uint32_t(std::clamp(GetInt("render_scale"), 1, 4) - 1);
    case 1: {
      int32_t filtering = GetInt("anisotropic_filtering");
      return filtering >= 5 ? 3 : filtering == 4 ? 2 : filtering == 3 ? 1 : 0;
    }
    case 2:
      return uint32_t(std::clamp(GetInt("bloom"), 0, 2));
    default:  // On, Off
      return GetSetting(kRows[row].setting) == "true" ? 0 : 1;
  }
}

void SetSetting(const char* name, const std::string& text) {
  if (GetSetting(name) == text) {
    return;
  }
  g_written[name] = text;
  // The window belongs to the UI thread.
  if (std::string_view(name) == "fullscreen" && g_appContext) {
    g_appContext->CallInUIThread([text] { rex::cvar::SetFlagByName("fullscreen", text); });
    return;
  }
  rex::cvar::SetFlagByName(name, text);
}

void SetValue(uint32_t row, uint32_t value) {
  switch (row) {
    case 0:
      SetSetting(kRows[row].setting, std::to_string(value + 1));
      break;
    case 1:
      SetSetting(kRows[row].setting, std::to_string(kFiltering[value]));
      break;
    case 2:
      SetSetting(kRows[row].setting, std::to_string(value));
      break;
    default:
      SetSetting(kRows[row].setting, value == 0 ? "true" : "false");
      break;
  }
}

void RestoreDefault(const Row& row) {
  if (const auto* info = rex::cvar::GetFlagInfo(row.setting)) {
    SetSetting(row.setting, info->default_value);
  }
}

void RestoreDefaults() {
  for (const Row& row : kRows) {
    RestoreDefault(row);
  }
}

}  // namespace

void SetConfigPath(const std::filesystem::path& path) {
  g_configPath = path;
}

void SetAppContext(rex::ui::WindowedAppContext* context) {
  g_appContext = context;
}

}  // namespace ng2::recomp_settings

using namespace ng2::recomp_settings;

namespace {

// Calls a guest function with r3-r8 below the caller's guest stack (the
// replaced handler has no frame of its own), leaving the caller's registers
// as they were.
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*),
                   std::initializer_list<uint32_t> args, double f1 = 0.0) {
  PPCContext saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  uint64_t* registers[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64,
                           &ctx.r6.u64, &ctx.r7.u64, &ctx.r8.u64};
  size_t i = 0;
  for (uint32_t arg : args) {
    *registers[i++] = arg;
  }
  ctx.f1.f64 = f1;
  function(ctx, base);
  uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// Where a row of the page being built goes: one of ours (its index), Restore
// Defaults (kSettingRows), or left out (-1).
int RowOf(uint32_t offset) {
  for (uint32_t i = 0; i < kSettingRows; i++) {
    if (offset == kChoiceRows[i]) {
      return int(i);
    }
  }
  return offset == kRestoreRow ? int(kSettingRows) : -1;
}

// The first time the builder hands over a row, after placing it: rows below
// rows left out move up by the space those took.
bool FirstSeen(uint32_t offset) {
  for (const Placed& placed : g_placed) {
    if (placed.offset == offset) {
      return false;
    }
  }
  uint32_t address = g_building + offset + kRowY;
  float y = LoadFloat(address);
  if (g_lastSkipped) {
    g_shift += y - g_lastY;
  }
  g_placed.push_back({offset, y});
  g_lastSkipped = RowOf(offset) < 0;
  g_lastY = y;
  if (!g_lastSkipped) {
    StoreFloat(address, y - g_shift);
  }
  return true;
}

}  // namespace

REX_EXTERN(__imp__sub_823E3448);  // a pad direction: (pad state, pad, 1, 0, 0 up 1 down 2 right 3 left)
REX_EXTERN(__imp__sub_823E3D78);  // Confirm: (&pad state, cursor, enabled[], 0)
REX_EXTERN(__imp__sub_823E3ED0);  // Back: (&pad state, cursor, 0xFF, 0)
REX_EXTERN(__imp__sub_823E3FD0);  // Y, a row's default: (&pad state)
REX_EXTERN(__imp__sub_82429DA8);  // a menu sound: (id), f1
REX_EXTERN(__imp__sub_83682750);  // a menu transition: (n, flag)
REX_EXTERN(__imp__sub_83681830);  // the page's state into the menu model

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

// The loop starts with the first rows picked, and leaves the game's as they
// were.
REX_EXTERN(__imp__sub_823E5778);
REX_HOOK_RAW(sub_823E5778) {
  uint32_t saved[2] = {ng2::guest::Load32(kCursors), ng2::guest::Load32(kCursors + 4 * kGamePage)};
  bool ours = g_open;
  if (ours) {
    Store32(kCursors, 0);
    Store32(kCursors + 4 * kGamePage, 0);
    g_entered = false;
  }
  __imp__sub_823E5778(ctx, base);
  if (ours) {
    // The game's Options keeps its own rows.
    Store32(kCursors, saved[0]);
    Store32(kCursors + 4 * kGamePage, saved[1]);
  }
}

// The category list, replaced while ours is open: its first frame picks
// Recomp Settings as the list's Confirm does, so the page comes in as usual;
// coming back to it leaves Options.
REX_EXTERN(__imp__sub_823E5B60);
REX_HOOK_RAW(sub_823E5B60) {
  if (!g_open) {
    __imp__sub_823E5B60(ctx, base);
    return;
  }
  using ng2::guest::Load32;
  uint32_t page = Load32(kPage);
  if (!g_entered) {
    g_entered = true;
    Store32(kCursors, 0);
    Store32(kPage, kGamePage);
    Store32(kDepth, Load32(kDepth) + 1);
    CallGuest(ctx, base, __imp__sub_83682750, {kEnter, 0});
  } else {
    Store32(kPage, kClosed);
    CallGuest(ctx, base, __imp__sub_83682750, {kLeave, 0});
  }
  uint32_t values = ctx.r1.u32 - 0x80;
  for (uint32_t i = 0; i < 8; i++) {
    Store32(values + 4 * i, 0);
  }
  CallGuest(ctx, base, __imp__sub_83681830,
            {kMenuModel, Load32(kLastPage), page, kCursors, values, 8});
}

// The Game Settings page's builder, run once as Options opens.
REX_EXTERN(__imp__sub_836AC710);
REX_HOOK_RAW(sub_836AC710) {
  if (!g_open) {
    __imp__sub_836AC710(ctx, base);
    return;
  }
  g_building = ctx.r3.u32;
  g_placed.clear();
  g_shift = 0.0f;
  g_lastSkipped = false;
  // The builder sets each row's choice from the page's model slots, in the
  // game's row order (ours are its rows 1-5). The game's are put back on
  // closing, for its own Options.
  g_modelSlots = ctx.r4.u32 + 0xB0;
  uint8_t* slots = ng2::guest::Virtual(g_modelSlots);
  g_savedModel.assign(slots, slots + 0x40);
  for (uint32_t i = 0; i < kSettingRows; i++) {
    Store32(ctx.r4.u32 + 0xB0 + 8 * (i + 1), ValueOf(i));
  }
  __imp__sub_836AC710(ctx, base);
  g_building = 0;
}

// A choice of a row: our row's choices on its first, nothing after.
REX_EXTERN(__imp__sub_836AA270);
REX_HOOK_RAW(sub_836AA270) {
  uint32_t offset = ctx.r3.u32 - g_building;
  if (!g_building || offset >= kRestoreRow + 0x100) {
    __imp__sub_836AA270(ctx, base);
    return;
  }
  bool first = FirstSeen(offset);
  int row = RowOf(offset);
  if (row < 0 || row >= int(kSettingRows)) {
    __imp__sub_836AA270(ctx, base);
    return;
  }
  if (!first) {
    return;
  }
  // Values are choice indices: not the game's On/Off rows (1 for the first).
  Store32(ctx.r3.u32 + kOnOffRow, 0);
  for (uint32_t i = 0; i < kRows[row].count; i++) {
    CallGuest(ctx, base, __imp__sub_836AA270, {ctx.r3.u32, TextId(kRows[row].choices[i])});
  }
}

// A row onto the page: ours under our label, the others left out.
REX_EXTERN(__imp__sub_836AB540);
REX_HOOK_RAW(sub_836AB540) {
  uint32_t offset = ctx.r5.u32 - g_building;
  if (!g_building || ctx.r3.u32 != g_building || offset >= kRestoreRow + 0x100) {
    __imp__sub_836AB540(ctx, base);
    return;
  }
  FirstSeen(offset);
  int row = RowOf(offset);
  if (row < 0) {
    return;
  }
  ctx.r4.u64 = TextId(row < int(kSettingRows) ? kRows[row].label : kRestoreText);
  __imp__sub_836AB540(ctx, base);
}

// The Game Settings page's handler, replaced while ours is open.
REX_EXTERN(__imp__sub_823E6490);
REX_HOOK_RAW(sub_823E6490) {
  if (!g_open) {
    __imp__sub_823E6490(ctx, base);
    return;
  }
  using ng2::guest::Load32;
  uint32_t page = Load32(kPage);
  uint32_t cursorAddress = kCursors + 4 * page;
  uint32_t cursor = std::min(Load32(cursorAddress), kRowCount - 1);
  double sound = LoadFloat(kCursorSound);
  if (CallGuest(ctx, base, __imp__sub_823E3ED0, {kPadState, cursor, 0xFF, 0})) {
    // Back leaves Options, as from the category list.
    Store32(kPage, kClosed);
    CallGuest(ctx, base, __imp__sub_83682750, {kLeave, 0});
  } else {
    uint32_t pad = Load32(kPadState);
    uint32_t player = *ng2::guest::Virtual(kPlayerPad);
    auto pressed = [&](uint32_t direction) {
      return CallGuest(ctx, base, __imp__sub_823E3448, {pad, player, 1, 0, direction}) != 0;
    };
    uint32_t moved = cursor;
    if (pressed(0)) {
      moved = (moved + kRowCount - 1) % kRowCount;
    }
    if (pressed(1)) {
      moved = (moved + 1) % kRowCount;
    }
    if (moved != cursor) {
      CallGuest(ctx, base, __imp__sub_82429DA8, {2}, sound);
      cursor = moved;
      Store32(cursorAddress, cursor);
    }
    if (cursor == kSettingRows) {
      // Restore Defaults; the enabled[] list is only read for the list page.
      if (CallGuest(ctx, base, __imp__sub_823E3D78, {kPadState, cursor, 0, 0})) {
        RestoreDefaults();
        CallGuest(ctx, base, __imp__sub_83682750, {kEnter, 0});
      }
    } else {
      uint32_t count = kRows[cursor].count;
      uint32_t value = ValueOf(cursor);
      uint32_t changed = value;
      bool left = pressed(3);
      if (left) {
        changed = (changed + count - 1) % count;
      }
      if (pressed(2)) {
        changed = (changed + 1) % count;
      }
      if (changed != value) {
        CallGuest(ctx, base, __imp__sub_82429DA8, {3}, sound);
        SetValue(cursor, changed);
      } else if (CallGuest(ctx, base, __imp__sub_823E3FD0, {kPadState})) {
        RestoreDefault(kRows[cursor]);
      }
      CallGuest(ctx, base, __imp__sub_83682750, {kChoiceChanged, left ? 0u : 1u});
    }
  }
  // The rows' values, below the guest stack, for the menu model.
  uint32_t values = ctx.r1.u32 - 0x80;
  for (uint32_t i = 0; i < 8; i++) {
    Store32(values + 4 * i, i < kSettingRows ? ValueOf(i) : 0);
  }
  CallGuest(ctx, base, __imp__sub_83681830,
            {kMenuModel, Load32(kLastPage), page, kCursors, values, 8});
}

// The Game Settings page's play time, formatted each frame into a shared
// string: blank on ours.
REX_EXTERN(__imp__sub_836AC0C8);
REX_HOOK_RAW(sub_836AC0C8) {
  __imp__sub_836AC0C8(ctx, base);
  if (g_open) {
    Store32(kPlayTimeText, 0);
  }
}
