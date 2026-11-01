// ng2 - Ninja Gaiden II static recompilation
//
// RECOMP SETTINGS: the title menu's Xbox LIVE Marketplace entry opens the
// game's own Options screen as a screen of the recomp's settings
// (docs/menus.md). While it is open:
//
// - The category list holds our categories (their names written over the
//   game's in the loaded string table, the rest blanked) and is run by us:
//   Confirm opens a category's page, Back leaves Options.
// - Each category is one of the game's pages. Its builder runs with its own
//   rows left out, then we append ours: arrow lists for settings with more
//   than two choices (made in place where the page has none), two-choice
//   rows for the others, and Restore Defaults. Our texts go in spare slots
//   of the string table (the Music Test's track names).
// - The pages' handlers are replaced by one that edits our settings, using
//   the game's own input, sound and menu-model functions.
//
// Everything is restored and the settings are saved when Options closes.
// The title menu also loses LEADERBOARDS and gains EXIT GAME here.

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
#include <rex/ppc/func.h>
#include <rex/ui/windowed_app_context.h>

#include <SDL3/SDL.h>

#include "gpu/guest.h"
#include "gpu/menu_label.h"
#include "recomp_settings.h"

REXCVAR_DEFINE_INT32(windowed_width, 0, "UI/Window",
                     "The window's width when not fullscreen (0: the default size)");
REXCVAR_DEFINE_INT32(windowed_height, 0, "UI/Window",
                     "The window's height when not fullscreen (0: the default size)");
REXCVAR_DEFINE_BOOL(exclusive_fullscreen, false, "UI/Window",
                    "Fullscreen in the desktop's display mode (exclusive) rather than as a "
                    "borderless window over the desktop");

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
constexpr uint32_t kCategoryPage = 0;
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

// The pages we use, and where in their page objects our rows go. Graphics
// is the Game Settings page: its language list is our first arrow row, and
// two more list widgets are made in the memory of a choice row it no longer
// uses. Display is the Controller Settings page. Each row's widget is reset,
// placed and appended by us after the game's builder has run with its own
// rows left out.
constexpr uint32_t kListVtable = 0x82281010;  // an arrow list ("<- value ->")
constexpr uint32_t kListSize = 0x270;
constexpr uint32_t kListTexts = 0x20;        // up to 10 value texts
constexpr uint32_t kListShown = 0x234;       // the value text shown
constexpr uint32_t kLeftArrow = 0x50, kRightArrow = 0xE0;  // the arrows' sprites
constexpr uint32_t kSpriteX = 0x30;          // an arrow's offset across, from the
                                             // value (it grows towards it)
constexpr float kArrowSpread = 32.0f;
constexpr uint32_t kWidgetValue = 0x14;
constexpr uint32_t kWidgetX = 0x18, kWidgetY = 0x1C;
constexpr uint32_t kOnOffRow = 0x600;  // a choice row's flag: 1 shows its first choice
constexpr uint32_t kPlayTimeText = 0x859D8DC4;  // UTF-16, from sub_836AC0C8
constexpr uint32_t kPageRows = 0x10;           // a page object's row count
constexpr uint32_t kModelValues = 0x85313EB0;  // + 0x40 * page + 8 * row
constexpr uint32_t kStringTable = 0x83BBC138;  // for sub_823E0E60(table, language, id)

// Options strings: table 2 (id 0x20000 + index), strings back to back in
// 16-byte slots.
constexpr uint32_t kTable = 0x20000;
// The categories' names, by category: ours first, the others blanked.
constexpr uint32_t kCategoryNames[] = {0x00, 0x01, 0x97, 0x03, 0x02, 0x04, 0x05, 0x61};
constexpr uint32_t kPlayTimeName = 0x2F;
// The Music Test's track names: room for our texts.
constexpr uint32_t kSpareFirst = 0x63, kSpareEnd = 0x97;
constexpr uint32_t kSlotCount = 0x98;  // past the last index used

enum class Kind {
  kScale, kFiltering, kLevel, kOnOff, kPresent, kWindowMode, kWindowSize, kSamples
};
struct Setting {
  std::u16string_view label;
  std::array<std::u16string_view, 10> choices;
  uint32_t count;
  const char* name;
  Kind kind;
};
constexpr Setting kSettings[] = {
    // With what the game's 3D (1120x584) is drawn at, as the NGB recomp's.
    {u"Render Scale",
     {u"1x 1120x584", u"2x 2240x1168", u"3x 3360x1752", u"4x 4480x2336", u"5x 5600x2920",
      u"6x 6720x3504", u"7x 7840x4088", u"8x 8960x4672"},
     8, "render_scale", Kind::kScale},
    {u"Texture Filtering", {u"Default", u"4x", u"8x", u"16x"}, 4, "anisotropic_filtering",
     Kind::kFiltering},
    {u"Bloom", {u"Off", u"Reduced", u"On"}, 3, "bloom", Kind::kLevel},
    {u"Anti-Aliasing", {u"Off", u"2x MSAA", u"4x MSAA", u"8x MSAA"}, 4, "antialiasing",
     Kind::kSamples},
    {u"Window Mode", {u"Windowed", u"Borderless", u"Fullscreen"}, 3, "fullscreen",
     Kind::kWindowMode},
    {u"Window Size", {u"1280x720", u"1600x900", u"1920x1080", u"2560x1440", u"3840x2160"}, 5,
     "windowed_width", Kind::kWindowSize},
    {u"V-Sync", {u"Off", u"FIFO", u"Mailbox", u"Adaptive"}, 4, "present_mode", Kind::kPresent},
    {u"Show FPS", {u"On", u"Off"}, 2, "show_fps", Kind::kOnOff},
    {u"Infinite Health", {u"On", u"Off"}, 2, "cheat_infinite_health", Kind::kOnOff},
    {u"Infinite Ninpo", {u"On", u"Off"}, 2, "cheat_infinite_ninpo", Kind::kOnOff},
};
// Window Size's choices.
constexpr std::pair<int32_t, int32_t> kWindowSizes[] = {
    {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};

struct PageRow {
  uint32_t setting;
  uint32_t widget;  // offset in the page object
  bool list;        // an arrow list (else two choices)
  bool make;        // a list widget we make there
};
struct Page {
  std::u16string_view name;
  uint32_t page;   // its Options page (category + 1)
  uint32_t size;   // its page object's
  uint32_t restore;  // the Restore Defaults row's widget
  std::array<PageRow, 4> rows;
  uint32_t count;
};
constexpr Page kPages[] = {
    {u"Graphics", 1, 0x2BB0, 0x2AD0,
     {{{0, 0x1E0, true, false},
       {1, 0x450, true, true},
       {2, 0x450 + kListSize, true, true},
       {3, 0xA60, true, true}}},
     4},
    {u"Display", 2, 0x1CC0, 0x1A20,
     {{{4, 0x1E0, true, true},
       {5, 0x1E0 + kListSize, true, true},
       {6, 0x7F0, true, true},
       {7, 0xE00, false, false}}},
     4},
    // The Camera Settings page: its two four-choice rows.
    {u"Cheats", 3, 0x13E0, 0x1300,
     {{{8, 0x1E0, false, false},
       {9, 0x7F0, false, false}}},
     2},
};
constexpr uint32_t kPageCount = std::size(kPages);
constexpr std::u16string_view kRestoreText = u"Restore Defaults";

const Page* PageOf(uint32_t page) {
  for (const Page& p : kPages) {
    if (p.page == page) {
      return &p;
    }
  }
  return nullptr;
}

bool g_open = false;
// The settings changed while open, as set (fullscreen is applied later, on
// the UI thread): saved on closing.
std::map<std::string, std::string, std::less<>> g_written;
// Guest memory ours replaces: the game's values in the menu model for our
// pages (put back on closing), and the widgets our list widgets are made
// over (put back before the page is next built: the closing screen still
// draws them).
std::vector<std::pair<uint32_t, std::vector<uint8_t>>> g_savedGuest;
std::vector<std::pair<uint32_t, std::vector<uint8_t>>> g_savedWidgets;

void PutBack(std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& saved) {
  for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
    std::memcpy(guest::Virtual(it->first), it->second.data(), it->second.size());
  }
  saved.clear();
}
std::filesystem::path g_configPath;
rex::ui::WindowedAppContext* g_appContext = nullptr;
struct SavedBytes {
  uint32_t physical;
  std::vector<uint8_t> bytes;  // the game's
  std::vector<uint8_t> ours;   // what we wrote, to check before restoring
};
std::vector<SavedBytes> g_savedTexts;
std::vector<std::pair<std::u16string_view, uint32_t>> g_textIds;  // our texts' string ids

// The page object being built by the game's builder, whose rows are left out.
uint32_t g_building = 0;
uint32_t g_buildingSize = 0;

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
  for (const Setting& setting : kSettings) {
    texts.push_back(setting.label);
    texts.insert(texts.end(), setting.choices.begin(), setting.choices.begin() + setting.count);
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

void PutBackGame();

void Open() {
  if (g_open) {
    return;
  }
  g_open = true;
  PutBackGame();
  ng2::video::SetRecompHeader(true);
  auto slots = FindTextSlots();
  if (slots.empty()) {
    REXLOG_WARN("recomp settings: Options texts not found");
  } else if (slots.size() == kSlotCount) {
    // The spare slots first: the table is found by its first name.
    PlaceTexts(slots);
    for (uint32_t i = 0; i < std::size(kCategoryNames); i++) {
      WriteText(slots[kCategoryNames[i]], i < kPageCount ? kPages[i].name : u"");
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

// The game's texts and menu values back, once nothing draws ours: on the
// title menu's next build, or before Options is next built or opened.
void PutBackGame() {
  ng2::video::SetRecompHeader(false);
  // Last written first: a slot written twice gets the game's bytes back.
  for (auto it = g_savedTexts.rbegin(); it != g_savedTexts.rend(); ++it) {
    uint8_t* slot = guest::Physical(it->physical);
    if (std::memcmp(slot, it->ours.data(), it->ours.size()) == 0) {
      std::memcpy(slot, it->bytes.data(), it->bytes.size());
    }
  }
  g_savedTexts.clear();
  g_textIds.clear();
  PutBack(g_savedGuest);
}

void Close() {
  if (!g_open) {
    return;
  }
  g_open = false;
  if (!g_written.empty() && !g_configPath.empty()) {
    SaveSettings();
  }
  g_written.clear();
  REXLOG_INFO("recomp settings: closed");
}

constexpr const char* kExclusive = "exclusive_fullscreen";

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

uint32_t ValueOf(const Setting& setting) {
  switch (setting.kind) {
    case Kind::kScale:
      return uint32_t(std::clamp(GetInt(setting.name), 1, int32_t(setting.count)) - 1);
    case Kind::kFiltering: {
      int32_t filtering = GetInt(setting.name);
      return filtering >= 5 ? 3 : filtering == 4 ? 2 : filtering == 3 ? 1 : 0;
    }
    case Kind::kLevel:
      return uint32_t(std::clamp(GetInt(setting.name), 0, int32_t(setting.count) - 1));
    case Kind::kOnOff:
      return GetSetting(setting.name) == "true" ? 0 : 1;
    case Kind::kPresent:
      // vsync = false from older settings means off until present_mode is set.
      if (!g_written.contains(setting.name) &&
          rex::cvar::GetFlagSource(setting.name) == rex::cvar::Source::kDefault &&
          GetSetting("vsync") == "false") {
        return 0;
      }
      return uint32_t(std::clamp(GetInt(setting.name), 0, int32_t(setting.count) - 1));
    case Kind::kWindowMode:
      return GetSetting("fullscreen") != "true" ? 0 : GetSetting(kExclusive) == "true" ? 2 : 1;
    case Kind::kSamples: {
      int32_t samples = GetInt(setting.name);
      return samples >= 8 ? 3 : samples >= 4 ? 2 : samples >= 2 ? 1 : 0;
    }
    case Kind::kWindowSize:
      for (uint32_t i = 0; i < std::size(kWindowSizes); i++) {
        if (GetInt("windowed_width") == kWindowSizes[i].first &&
            GetInt("windowed_height") == kWindowSizes[i].second) {
          return i;
        }
      }
      return 0;
  }
  return 0;
}

// The window's settings, applied on the UI thread (the window belongs to
// it): the fullscreen display mode before fullscreen itself, the windowed
// size after leaving it.
bool IsWindowSetting(std::string_view name) {
  return name == "fullscreen" || name == kExclusive || name == "windowed_width" ||
         name == "windowed_height";
}

void ApplyWindow(bool fullscreen) {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  if (!window) {
    return;
  }
  if (fullscreen) {
    const SDL_DisplayMode* mode =
        rex::cvar::Query<bool>(kExclusive)
            ? SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window))
            : nullptr;
    SDL_SetWindowFullscreenMode(window, mode);
  }
  // Through the cvar: the runtime keeps its own fullscreen state.
  rex::cvar::SetFlagByName("fullscreen", fullscreen ? "true" : "false");
  if (!fullscreen) {
    int32_t width = rex::cvar::Query<int32_t>("windowed_width");
    int32_t height = rex::cvar::Query<int32_t>("windowed_height");
    if (width <= 0 || height <= 0) {
      width = kWindowSizes[0].first, height = kWindowSizes[0].second;
    }
    SDL_SetWindowSize(window, width, height);
  }
}

bool g_windowChanged = false;

void ScheduleWindow() {
  g_windowChanged = false;
  if (!g_appContext) {
    return;
  }
  std::vector<std::pair<std::string, std::string>> values;
  for (const auto& [name, value] : g_written) {
    if (IsWindowSetting(name) && name != "fullscreen") {
      values.emplace_back(name, value);
    }
  }
  bool fullscreen = GetSetting("fullscreen") == "true";
  g_appContext->CallInUIThread([values, fullscreen] {
    for (const auto& [name, value] : values) {
      rex::cvar::SetFlagByName(name, value);
    }
    ApplyWindow(fullscreen);
  });
}

void SetSetting(const char* name, const std::string& text) {
  if (GetSetting(name) == text) {
    return;
  }
  g_written[name] = text;
  if (IsWindowSetting(name)) {
    g_windowChanged = true;  // applied once the row's settings are all set
    return;
  }
  rex::cvar::SetFlagByName(name, text);
}

void SetValue(const Setting& setting, uint32_t value) {
  switch (setting.kind) {
    case Kind::kScale:
      SetSetting(setting.name, std::to_string(value + 1));
      break;
    case Kind::kFiltering:
      SetSetting(setting.name, std::to_string(kFiltering[value]));
      break;
    case Kind::kLevel:
      SetSetting(setting.name, std::to_string(value));
      break;
    case Kind::kOnOff:
      SetSetting(setting.name, value == 0 ? "true" : "false");
      break;
    case Kind::kPresent:
      // The older vsync setting would keep it off otherwise. It is also the
      // SDK's guest vblank switch (off: 1000 Hz, which the game would run
      // at), so it stays on.
      SetSetting("vsync", "true");
      SetSetting(setting.name, std::to_string(value));
      break;
    case Kind::kWindowMode:
      SetSetting(kExclusive, value == 2 ? "true" : "false");
      SetSetting("fullscreen", value != 0 ? "true" : "false");
      break;
    case Kind::kSamples:
      SetSetting(setting.name, std::to_string(value == 0 ? 0 : 1 << value));
      break;
    case Kind::kWindowSize:
      SetSetting("windowed_width", std::to_string(kWindowSizes[value].first));
      SetSetting("windowed_height", std::to_string(kWindowSizes[value].second));
      break;
  }
}

void RestoreDefault(const Setting& setting) {
  auto reset = [](const char* name) {
    if (const auto* info = rex::cvar::GetFlagInfo(name)) {
      SetSetting(name, info->default_value);
    }
  };
  reset(setting.name);
  if (setting.kind == Kind::kPresent) {
    reset("vsync");
  } else if (setting.kind == Kind::kWindowMode) {
    reset(kExclusive);
  } else if (setting.kind == Kind::kWindowSize) {
    reset("windowed_height");
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

// The same, for a function returning a float.
double CallGuestFloat(PPCContext& ctx, uint8_t* base, void (*function)(PPCContext&, uint8_t*),
                      std::initializer_list<uint32_t> args) {
  PPCContext saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  uint64_t* registers[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
  size_t i = 0;
  for (uint32_t arg : args) {
    *registers[i++] = arg;
  }
  function(ctx, base);
  double result = ctx.f1.f64;
  ctx = saved;
  return result;
}

}  // namespace

REX_EXTERN(__imp__sub_823E3448);  // a pad direction: (pad state, pad, 1, 0, 0 up 1 down 2 right 3 left)
REX_EXTERN(__imp__sub_823E3D78);  // Confirm: (&pad state, cursor, enabled[], 0)
REX_EXTERN(__imp__sub_823E3ED0);  // Back: (&pad state, cursor, 0xFF, 0)
REX_EXTERN(__imp__sub_823E3FD0);  // Y, a row's default: (&pad state)
REX_EXTERN(__imp__sub_82429DA8);  // a menu sound: (id), f1
REX_EXTERN(__imp__sub_83682750);  // a menu transition: (n, flag)
REX_EXTERN(__imp__sub_83681830);  // the page's state into the menu model
REX_EXTERN(__imp__sub_836AA198);  // a widget's reset: (widget, model)
REX_EXTERN(__imp__sub_836AA260);  // a list's or label's height: (widget) -> f1
REX_EXTERN(__imp__sub_836AA488);  // a choice row's height: (widget) -> f1
REX_EXTERN(__imp__sub_836DFDE0);  // a text sprite's constructor: (sprite)
REX_EXTERN(__imp__sub_823E41F0);  // a widget part's constructor: (part)
REX_EXTERN(__imp__sub_823E0E60);  // a string: (table, language, id) -> pointer
REX_EXTERN(__imp__sub_8365CDF0);  // the text language
REX_EXTERN(__imp__sub_836AA270);  // a choice onto a choice row: (widget, string id)
REX_EXTERN(__imp__sub_836AB540);  // a row onto a page: (page, label id, widget)

// The title menu's entries (sub_8364CBE8): 0x140 bytes each, from
// 0x85387600; at +0x124 the action, +0x128 the row's place, +0x130 "needs
// Xbox LIVE" (shown greyed while signed out). LEADERBOARDS (action 3) goes,
// the entries below move up into the rows above, and ours isn't greyed.
constexpr uint32_t kTitleEntries = 0x85387600;
constexpr uint32_t kTitleEntrySize = 0x140;
constexpr uint32_t kTitleEntryCount = 0x85388500;
constexpr uint32_t kEntryAction = 0x124, kEntryPlace = 0x128, kEntryOnline = 0x130;
constexpr uint32_t kLeaderboards = 3, kMarketplace = 6;
// A new last entry quits the game: action 7, which the title menu doesn't
// handle; its picture (table 0x83A93700, by language and action) is the
// atlas's EXIT DEMO band, redrawn as EXIT GAME (src/gpu/menu_label.cpp).
constexpr uint32_t kExitAction = 7;
constexpr uint32_t kEntryStoragePrompt = 0x12C;
constexpr uint32_t kTitleEntryCapacity = 12;
constexpr uint32_t kTitleCursor = 0x85388504;
constexpr uint32_t kPadPressed = 0x83BF1F78;  // + pad * 0x44: newly pressed buttons
constexpr uint32_t kConfirmButtons = 0x150;

REX_EXTERN(__imp__sub_8364CBE8);
REX_HOOK_RAW(sub_8364CBE8) {
  PutBackGame();
  __imp__sub_8364CBE8(ctx, base);
  using ng2::guest::Load32;
  uint32_t count = Load32(kTitleEntryCount);
  if (count > 8) {
    return;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint32_t entry = kTitleEntries + i * kTitleEntrySize;
    if (Load32(entry + kEntryAction) == kMarketplace) {
      Store32(entry + kEntryOnline, 0);
    }
  }
  for (uint32_t i = 0; i < count; i++) {
    uint32_t entry = kTitleEntries + i * kTitleEntrySize;
    if (Load32(entry + kEntryAction) != kLeaderboards) {
      continue;
    }
    for (uint32_t j = i; j + 1 < count; j++) {
      uint32_t to = kTitleEntries + j * kTitleEntrySize;
      uint32_t place = Load32(to + kEntryPlace);
      std::memcpy(ng2::guest::Virtual(to), ng2::guest::Virtual(to + kTitleEntrySize),
                  kTitleEntrySize);
      Store32(to + kEntryPlace, place);
    }
    Store32(kTitleEntryCount, --count);
    break;
  }
  if (count >= 2 && count < kTitleEntryCapacity) {
    uint32_t last = kTitleEntries + (count - 1) * kTitleEntrySize;
    uint32_t exit = last + kTitleEntrySize;
    std::memcpy(ng2::guest::Virtual(exit), ng2::guest::Virtual(last), kTitleEntrySize);
    Store32(exit + kEntryAction, kExitAction);
    Store32(exit + kEntryPlace,
            Load32(last + kEntryPlace) * 2 - Load32(last - kTitleEntrySize + kEntryPlace));
    Store32(exit + kEntryStoragePrompt, 0);
    Store32(exit + kEntryOnline, 0);
    Store32(kTitleEntryCount, count + 1);
  }
}

// The title menu's "signed in" check: ours always takes the signed-out path,
// whose message sub_8364CFC8 turns into our screen.
REX_EXTERN(__imp__sub_8364CA08);
REX_HOOK_RAW(sub_8364CA08) {
  uint32_t index = ctx.r3.u32;
  if (index < ng2::guest::Load32(kTitleEntryCount) &&
      ng2::guest::Load32(kTitleEntries + index * kTitleEntrySize + kEntryAction) == kMarketplace) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8364CA08(ctx, base);
}

// The title menu's input: Confirm on the exit entry quits.
REX_EXTERN(__imp__sub_8364D0C0);
REX_HOOK_RAW(sub_8364D0C0) {
  __imp__sub_8364D0C0(ctx, base);
  using ng2::guest::Load32;
  uint32_t player = *ng2::guest::Virtual(kPlayerPad);
  uint32_t cursor = Load32(kTitleCursor);
  if ((Load32(kPadPressed + player * 0x44) & kConfirmButtons) && cursor < Load32(kTitleEntryCount) &&
      Load32(kTitleEntries + cursor * kTitleEntrySize + kEntryAction) == kExitAction &&
      g_appContext) {
    REXLOG_INFO("title menu: exit");
    // As the window's close button: the runtime ends the title and exits
    // (a plain quit runs a teardown that can wait on the game's threads).
    g_appContext->CallInUIThread([] {
      int count = 0;
      SDL_Window** windows = SDL_GetWindows(&count);
      SDL_Window* window = (windows && count > 0) ? windows[0] : nullptr;
      SDL_free(windows);
      if (window) {
        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        event.window.windowID = SDL_GetWindowID(window);
        SDL_PushEvent(&event);
      }
    });
  }
}

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


namespace {

// The values of a page's rows (ours, then Restore Defaults' 0), below the
// guest stack.
uint32_t StoreValues(PPCContext& ctx, const Page* page) {
  uint32_t values = ctx.r1.u32 - 0x80;
  for (uint32_t i = 0; i < 8; i++) {
    Store32(values + 4 * i, page && i < page->count ? ValueOf(kSettings[page->rows[i].setting]) : 0);
  }
  return values;
}

// A list widget made in place, as the Game Settings page's constructor
// (sub_83681EB0) makes its language list.
void MakeList(PPCContext& ctx, uint8_t* base, uint32_t widget) {
  uint8_t* bytes = ng2::guest::Virtual(widget);
  g_savedWidgets.emplace_back(widget, std::vector<uint8_t>(bytes, bytes + kListSize));
  std::memset(ng2::guest::Virtual(widget), 0, kListSize);
  Store32(widget, kListVtable);
  for (uint32_t sprite : {0x50u, 0xE0u, 0x170u}) {
    Store32(widget + sprite + 0x74, 0);
    CallGuest(ctx, base, __imp__sub_836DFDE0, {widget + sprite});
  }
  CallGuest(ctx, base, __imp__sub_823E41F0, {widget + 0x200});
  Store32(widget + 0x260, 1);
}

// Our rows onto a page object, after its builder (model: the builder's
// second argument, the menu model + 0x10).
void BuildPage(PPCContext& ctx, uint8_t* base, const Page& page, uint32_t object, uint32_t model) {
  using ng2::guest::Load32;
  Store32(object + kPageRows, 0);
  uint32_t language = CallGuest(ctx, base, __imp__sub_8365CDF0, {}) & 0xFF;
  uint32_t table = Load32(kStringTable);
  auto text = [&](std::u16string_view s) {
    return CallGuest(ctx, base, __imp__sub_823E0E60, {table, language, TextId(s)});
  };
  float y = 0.0f;
  auto place = [&](uint32_t widget) {
    CallGuest(ctx, base, __imp__sub_836AA198, {widget, model});
    StoreFloat(widget + kWidgetX, 0.0f);
    StoreFloat(widget + kWidgetY, y);
  };
  for (uint32_t i = 0; i < page.count; i++) {
    const PageRow& row = page.rows[i];
    const Setting& setting = kSettings[row.setting];
    uint32_t widget = object + row.widget;
    uint32_t value = ValueOf(setting);
    if (row.make) {
      MakeList(ctx, base, widget);
    }
    place(widget);
    if (row.list) {
      for (uint32_t k = 0; k < 10; k++) {
        Store32(widget + kListTexts + 4 * k, text(setting.choices[std::min(k, setting.count - 1)]));
      }
      Store32(widget + kListShown, Load32(widget + kListTexts + 4 * value));
      // The arrows further apart than the game's, for the longer values
      // (Render Scale's resolutions).
      StoreFloat(widget + kLeftArrow + kSpriteX, LoadFloat(widget + kLeftArrow + kSpriteX) + kArrowSpread);
      StoreFloat(widget + kRightArrow + kSpriteX, LoadFloat(widget + kRightArrow + kSpriteX) - kArrowSpread);
      // The list's parts are placed for the page's first row only.
      for (uint32_t part : {0x50u + 0x34u, 0xE0u + 0x34u, 0x244u}) {
        StoreFloat(widget + part, LoadFloat(widget + part) + y);
      }
    } else {
      Store32(widget + kOnOffRow, 0);
      for (uint32_t k = 0; k < setting.count; k++) {
        CallGuest(ctx, base, __imp__sub_836AA270, {widget, TextId(setting.choices[k])});
      }
    }
    Store32(widget + kWidgetValue, value);
    CallGuest(ctx, base, __imp__sub_836AB540, {object, TextId(setting.label), widget});
    y += float(CallGuestFloat(ctx, base, row.list ? __imp__sub_836AA260 : __imp__sub_836AA488,
                              {widget}));
  }
  uint32_t restore = object + page.restore;
  place(restore);
  CallGuest(ctx, base, __imp__sub_836AB540, {object, TextId(kRestoreText), restore});
  // The rows' values in the model, as the handler will keep them; the
  // game's are put back on closing.
  uint32_t slots = kModelValues + 0x40 * page.page;
  uint8_t* bytes = ng2::guest::Virtual(slots);
  g_savedGuest.emplace_back(slots, std::vector<uint8_t>(bytes, bytes + 0x40));
  for (uint32_t i = 0; i < 8; i++) {
    uint32_t value = i < page.count ? ValueOf(kSettings[page.rows[i].setting]) : 0;
    Store32(slots + 8 * i, value);
    Store32(slots + 8 * i + 4, value);
  }
}

// A page's builder, run once as Options opens: the game's rows left out,
// then ours.
void Build(PPCContext& ctx, uint8_t* base, void (*builder)(PPCContext&, uint8_t*), uint32_t number) {
  uint32_t object = ctx.r3.u32, model = ctx.r4.u32;
  const Page* page = PageOf(number);
  if (!g_open) {
    PutBackGame();
  }
  std::erase_if(g_savedWidgets, [&](auto& saved) {
    if (saved.first - object >= page->size) {
      return false;
    }
    std::memcpy(ng2::guest::Virtual(saved.first), saved.second.data(), saved.second.size());
    return true;
  });
  if (!g_open) {
    builder(ctx, base);
    return;
  }
  g_building = object;
  g_buildingSize = page->size;
  builder(ctx, base);
  g_building = 0;
  BuildPage(ctx, base, *page, object, model);
}

bool InBuild(uint32_t address) {
  return g_building && address - g_building < g_buildingSize;
}

// A page's input, while ours is open: up and down pick a row, left and right
// change it, Y sets its default, Confirm on Restore Defaults sets them all,
// Back returns to the category list.
void RunPage(PPCContext& ctx, uint8_t* base, const Page& page) {
  using ng2::guest::Load32;
  uint32_t number = Load32(kPage);
  uint32_t rowCount = page.count + 1;
  uint32_t cursorAddress = kCursors + 4 * number;
  uint32_t cursor = std::min(Load32(cursorAddress), rowCount - 1);
  double sound = LoadFloat(kCursorSound);
  if (CallGuest(ctx, base, __imp__sub_823E3ED0, {kPadState, cursor, 0xFF, 0})) {
    Store32(kPage, kCategoryPage);
    Store32(kDepth, Load32(kDepth) - 1);
    CallGuest(ctx, base, __imp__sub_83682750, {kLeave, 0});
  } else {
    uint32_t pad = Load32(kPadState);
    uint32_t player = *ng2::guest::Virtual(kPlayerPad);
    auto pressed = [&](uint32_t direction) {
      return CallGuest(ctx, base, __imp__sub_823E3448, {pad, player, 1, 0, direction}) != 0;
    };
    uint32_t moved = cursor;
    if (pressed(0)) {
      moved = (moved + rowCount - 1) % rowCount;
    }
    if (pressed(1)) {
      moved = (moved + 1) % rowCount;
    }
    if (moved != cursor) {
      CallGuest(ctx, base, __imp__sub_82429DA8, {2}, sound);
      cursor = moved;
      Store32(cursorAddress, cursor);
    }
    if (cursor == page.count) {
      // Restore Defaults; the enabled[] list is only read for the list page.
      if (CallGuest(ctx, base, __imp__sub_823E3D78, {kPadState, cursor, 0, 0})) {
        for (uint32_t i = 0; i < page.count; i++) {
          RestoreDefault(kSettings[page.rows[i].setting]);
        }
        CallGuest(ctx, base, __imp__sub_83682750, {kEnter, 0});
      }
    } else {
      const Setting& setting = kSettings[page.rows[cursor].setting];
      uint32_t value = ValueOf(setting);
      uint32_t changed = value;
      bool left = pressed(3);
      if (left) {
        changed = (changed + setting.count - 1) % setting.count;
      }
      if (pressed(2)) {
        changed = (changed + 1) % setting.count;
      }
      if (changed != value) {
        CallGuest(ctx, base, __imp__sub_82429DA8, {3}, sound);
        SetValue(setting, changed);
      } else if (CallGuest(ctx, base, __imp__sub_823E3FD0, {kPadState})) {
        RestoreDefault(setting);
      }
      CallGuest(ctx, base, __imp__sub_83682750, {kChoiceChanged, left ? 0u : 1u});
    }
  }
  if (g_windowChanged) {
    ScheduleWindow();
  }
  CallGuest(ctx, base, __imp__sub_83681830,
            {kMenuModel, Load32(kLastPage), number, kCursors, StoreValues(ctx, &page), 8});
}

}  // namespace

// The loop starts with the first category and rows picked, and leaves the
// game's as they were.
REX_EXTERN(__imp__sub_823E5778);
REX_HOOK_RAW(sub_823E5778) {
  constexpr uint32_t kSaved = kPageCount + 1;
  uint32_t saved[kSaved];
  for (uint32_t i = 0; i < kSaved; i++) {
    saved[i] = ng2::guest::Load32(kCursors + 4 * i);
  }
  bool ours = g_open;
  if (ours) {
    for (uint32_t i = 0; i < kSaved; i++) {
      Store32(kCursors + 4 * i, 0);
    }
  }
  __imp__sub_823E5778(ctx, base);
  if (ours) {
    for (uint32_t i = 0; i < kSaved; i++) {
      Store32(kCursors + 4 * i, saved[i]);
    }
  }
}

// The category list, replaced while ours is open: our categories only;
// Confirm opens one's page, Back leaves Options.
REX_EXTERN(__imp__sub_823E5B60);
REX_HOOK_RAW(sub_823E5B60) {
  if (!g_open) {
    __imp__sub_823E5B60(ctx, base);
    return;
  }
  using ng2::guest::Load32;
  uint32_t page = Load32(kPage);
  uint32_t cursor = std::min(Load32(kCursors), kPageCount - 1);
  uint32_t enabled = ctx.r1.u32 - 0xC0;
  for (uint32_t i = 0; i < 8; i++) {
    Store32(enabled + 4 * i, i < kPageCount);
  }
  if (CallGuest(ctx, base, __imp__sub_823E3D78, {kPadState, cursor, enabled, 0})) {
    Store32(kPage, kPages[cursor].page);
    Store32(kDepth, Load32(kDepth) + 1);
    CallGuest(ctx, base, __imp__sub_83682750, {kEnter, 0});
  } else if (CallGuest(ctx, base, __imp__sub_823E3ED0, {kPadState, cursor, 7, 0})) {
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
      moved = (moved + kPageCount - 1) % kPageCount;
    }
    if (pressed(1)) {
      moved = (moved + 1) % kPageCount;
    }
    if (moved != cursor) {
      CallGuest(ctx, base, __imp__sub_82429DA8, {2}, LoadFloat(kCursorSound));
    }
    Store32(kCursors, moved);
  }
  CallGuest(ctx, base, __imp__sub_83681830,
            {kMenuModel, Load32(kLastPage), page, kCursors, StoreValues(ctx, nullptr), 8});
}

// The pages' builders.
REX_EXTERN(__imp__sub_836AC710);
REX_HOOK_RAW(sub_836AC710) {
  Build(ctx, base, __imp__sub_836AC710, 1);  // Game Settings
}
REX_EXTERN(__imp__sub_836AD330);
REX_HOOK_RAW(sub_836AD330) {
  Build(ctx, base, __imp__sub_836AD330, 2);  // Controller Settings
}
REX_EXTERN(__imp__sub_836ADE10);
REX_HOOK_RAW(sub_836ADE10) {
  Build(ctx, base, __imp__sub_836ADE10, 3);  // Camera Settings
}

// A choice or a row from a builder whose rows are left out.
REX_HOOK_RAW(sub_836AA270) {
  if (!InBuild(ctx.r3.u32)) {
    __imp__sub_836AA270(ctx, base);
  }
}
REX_HOOK_RAW(sub_836AB540) {
  if (!InBuild(ctx.r3.u32)) {
    __imp__sub_836AB540(ctx, base);
  }
}

// The pages' handlers, replaced while ours is open.
REX_EXTERN(__imp__sub_823E6490);
REX_HOOK_RAW(sub_823E6490) {
  if (g_open) {
    RunPage(ctx, base, kPages[0]);
  } else {
    __imp__sub_823E6490(ctx, base);
  }
}
REX_EXTERN(__imp__sub_823E6AD8);
REX_HOOK_RAW(sub_823E6AD8) {
  if (g_open) {
    RunPage(ctx, base, kPages[1]);
  } else {
    __imp__sub_823E6AD8(ctx, base);
  }
}
REX_EXTERN(__imp__sub_823E7040);
REX_HOOK_RAW(sub_823E7040) {
  if (g_open) {
    RunPage(ctx, base, kPages[2]);
  } else {
    __imp__sub_823E7040(ctx, base);
  }
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

void ng2::recomp_settings::ApplyWindowAtStart() {
  // vsync = false from older builds' V-Sync Off: it is also the SDK's guest
  // vblank switch (off: 1000 Hz), so it becomes present_mode 0 and vsync on.
  if (GetSetting("vsync") == "false") {
    if (rex::cvar::GetFlagSource("present_mode") == rex::cvar::Source::kDefault) {
      SetSetting("present_mode", "0");
    }
    SetSetting("vsync", "true");
    if (!g_configPath.empty()) {
      SaveSettings();
    }
    g_written.clear();
  }
  // The runtime opens the window fullscreen as a borderless window, or at its
  // default size: the exclusive mode or our windowed size is set over it.
  bool fullscreen = rex::cvar::Query<bool>("fullscreen");
  if (g_appContext && (fullscreen ? rex::cvar::Query<bool>(kExclusive)
                                  : rex::cvar::Query<int32_t>("windowed_width") > 0)) {
    g_appContext->CallInUIThread([fullscreen] { ApplyWindow(fullscreen); });
  }
}

// RECOMP SETTINGS in gameplay: the Back button's "IN-GAME MANUAL AND OPTIONS"
// list (docs/menus.md) gets a RECOMP SETTINGS row after its twelve. Choosing
// it lays the list out as the title's RECOMP SETTINGS screen until Back: the
// categories on the left, the chosen one's settings on the right ("< value >"
// in the game's font where the title has arrow lists), Restore Defaults, and
// the header redrawn as RECOMP SETTINGS. The list's twelve rows then hold the
// categories, the chosen one's settings, Restore Defaults and empty rows of
// no height. The list's own methods walk its twelve rows, so the extra row is
// added to them by the hooks below.
namespace {

constexpr uint32_t kInGameState = 0x83CEB3B4;    // 0: the list
constexpr uint32_t kInGameCursors = 0x83CEB3C4;  // + 4 * state
constexpr uint32_t kInGameModel = 0x8530B8C0;    // cursor, last cursor
constexpr uint32_t kInGameViews[] = {0x8530B930, 0x8530BB80, 0x85310AE0};  // told of moves
constexpr uint32_t kGameRows = 12;
constexpr uint32_t kEntryIndex = 12;   // our row's index, as the game's handler sees it
constexpr uint32_t kEntryShownAt = 10;   // where it's shown: after Save Method

// Our row is shown after Save Method, but the game's handler and the list
// keep their own indices (10 Quit Game, 11 Return to Game; ours is 12):
// only moving up and down goes by the order shown.
uint32_t ToShown(uint32_t index) {
  return index < kEntryShownAt ? index : index == kEntryIndex ? kEntryShownAt : index + 1;
}
uint32_t ToGameIndex(uint32_t shown) {
  return shown < kEntryShownAt ? shown : shown == kEntryShownAt ? kEntryIndex : shown - 1;
}
constexpr uint32_t kRowsAt = 8;                 // the list's row pointers
constexpr uint32_t kChoiceRowSize = 0x780, kLabelRowSize = 0x130;
constexpr uint32_t kLabelRowVtable = 0x82280C80;  // Return to Game's class
constexpr uint32_t kQuitRowVtable = 0x82280CC8;   // Quit Game's
constexpr uint32_t kRowLabel = 0xE4;              // a row's text
constexpr uint32_t kChoiceText = 0x434;           // + 0x100 per choice
constexpr uint32_t kRowHeight = 0x18;
constexpr uint32_t kLineHeight = 0x821821B4;      // the float the builder gives rows
constexpr uint32_t kAnyText = 0x40018;            // a string id, for the setup call
constexpr uint32_t kGuestAlloc = 0xB2800000;
constexpr uint32_t kChoicesShown = 4;             // a choice row's room

constexpr std::u16string_view kEntryText = u"Recomp Settings";
// Our layout in the list's rows: the categories, then a page's settings and
// Restore Defaults, drawn to the right of the categories (where the title
// screen has its pages), then empty rows.
constexpr uint32_t kFirstPageRow = kPageCount;
constexpr uint32_t kEmptyRows = kGameRows - kFirstPageRow - 1;  // for the shortest page
constexpr float kPageColumn = 344.0f;  // the title's pages are this far right of its categories
constexpr uint32_t kCursorMark = 0x4ED0, kCursorMarkSlot = 0x4EC0;  // in the list
constexpr uint32_t kMarkX = 0x30, kMarkY = 0x34;  // the mark's place, from the list's
constexpr uint32_t kMarkOrigin = 0x82284800;     // the float the list places it from
constexpr uint32_t kValueRoom = 24;
// A row is drawn where its slot's frame in the list's layout is; each of its
// sprites (label, choice mark, choices, the strokes between them) is placed
// from there by +0x30. The label's and the choices' are 0; the strokes' are
// set by the row's setup, the choice mark's (under the chosen one) by its
// update when the value changes.
constexpr uint32_t kSpriteOffset = 0x30;
constexpr uint32_t kRowSprite = 0x20, kChoiceMark = 0x120;
constexpr uint32_t kChoiceItems = 0x370, kChoiceItemSize = 0x100;
constexpr uint32_t kStrokes = 0x1C0, kStrokeSize = 0x90;
constexpr uint32_t kChoiceCount = 0x770;

uint32_t g_list = 0;            // the list object, once built
uint32_t g_arena = 0, g_arenaUsed = 0, g_arenaSize = 0;
uint32_t g_entry = 0;           // our row in the list
uint32_t g_settingRows[std::size(kSettings)] = {};
uint32_t g_valueText[std::size(kSettings)] = {};  // rows with too many choices show one
uint32_t g_categoryRows[kPageCount] = {}, g_restoreRow = 0, g_emptyRows[kEmptyRows] = {};
uint32_t g_category = 0;       // the category shown
bool g_inPage = false;         // the cursor is in its settings (else on the categories)
uint32_t g_gameRows[kGameRows] = {};
bool g_inGameOurs = false;      // our settings are in the list
uint32_t g_suppress = 0;        // directions (bit per sub_823E3448 direction) and Confirm (bit 4)

uint32_t ArenaAlloc(uint32_t size) {
  size = (size + 15) & ~15u;
  if (g_arenaUsed + size > g_arenaSize) {
    return 0;
  }
  uint32_t at = g_arena + g_arenaUsed;
  g_arenaUsed += size;
  return at;
}

// A UTF-16 string in guest memory, with room for chars more.
uint32_t GuestText(std::u16string_view text, uint32_t room = 0) {
  uint32_t chars = uint32_t(std::max<size_t>(text.size(), room)) + 1;
  uint32_t at = ArenaAlloc(chars * 2);
  if (at) {
    std::vector<uint8_t> bytes = Utf16BE(text);
    std::memset(ng2::guest::Virtual(at), 0, chars * 2);
    std::memcpy(ng2::guest::Virtual(at), bytes.data(), bytes.size());
  }
  return at;
}

void SetGuestText(uint32_t at, std::u16string_view text) {
  std::vector<uint8_t> bytes = Utf16BE(text);
  bytes.push_back(0), bytes.push_back(0);
  std::memcpy(ng2::guest::Virtual(at), bytes.data(), bytes.size());
}

}  // namespace

REX_EXTERN(__imp__sub_83810258);  // XMemAlloc(size, attributes)
REX_EXTERN(__imp__sub_8366E078);  // a choice row's constructor
REX_EXTERN(__imp__sub_8366FCA0);  // a choice row's setup: (row, label id, ids, count, reversed)
REX_EXTERN(__imp__sub_8366E238);  // a row's value: (row, value)
REX_EXTERN(__imp__sub_8366F158);  // an event to a view: (view, event, model)

namespace {

// A guest function by address (a vtable's), as generated code calls them.
void CallAt(PPCContext& ctx, uint8_t* base, uint32_t address, std::initializer_list<uint64_t> args) {
  PPCContext saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  uint64_t* registers[] = {&ctx.r3.u64, &ctx.r4.u64, &ctx.r5.u64, &ctx.r6.u64};
  size_t i = 0;
  for (uint64_t arg : args) {
    *registers[i++] = arg;
  }
  rex::runtime::ResolveIndirectFunction(address)(ctx, base);
  ctx = saved;
}

uint32_t Method(uint32_t object, uint32_t slot) {
  return ng2::guest::Load32(ng2::guest::Load32(object) + 4 * slot);
}

// A row as the list's builder sets one up: index, init, line height.
void InitRow(PPCContext& ctx, uint8_t* base, uint32_t row, uint32_t index, bool empty = false) {
  Store32(row + 8, index);
  Store32(row + 12, 0);
  Store32(row + 4, 0);
  CallAt(ctx, base, Method(row, 0), {row});
  Store32(row + 0x14, 0x1C);
  StoreFloat(row + kRowHeight, empty ? 0.0f : LoadFloat(kLineHeight));
}

// A label row, as the list's constructor makes Return to Game.
void MakeLabelRow(PPCContext& ctx, uint8_t* base, uint32_t row) {
  std::memset(ng2::guest::Virtual(row), 0, kLabelRowSize);
  Store32(row, kLabelRowVtable);
  CallGuest(ctx, base, __imp__sub_836DFDE0, {row + 0x20});
  CallGuest(ctx, base, __imp__sub_823E41F0, {row + 0xB0});
  Store32(row + 0x110, 1);
}

std::u16string ValueText(const Setting& setting) {
  return std::u16string(setting.choices[std::min(ValueOf(setting), setting.count - 1)]);
}

// Shown as "< value >", as the title shows it in an arrow list.
bool IsListSetting(uint32_t setting) {
  for (const Page& page : kPages) {
    for (uint32_t i = 0; i < page.count; i++) {
      if (page.rows[i].setting == setting) {
        return page.rows[i].list;
      }
    }
  }
  return false;
}

std::u16string ArrowText(const Setting& setting) {
  return u"\u2039  " + ValueText(setting) + u"  \u203A";
}

// Our rows and their texts, made once.
uint32_t g_ids = 0;  // string ids for the choice rows' setup call
uint32_t g_entryText = 0, g_restoreText = 0, g_emptyText = 0;
uint32_t g_categoryText[kPageCount] = {};
uint32_t g_labelText[std::size(kSettings)] = {};
uint32_t g_choiceTexts[std::size(kSettings)][kChoicesShown] = {};

bool MakeInGameRowsOnce(PPCContext& ctx, uint8_t* base) {
  if (g_arena) {
    return true;
  }
  constexpr uint32_t kSize = 0x8000;
  uint32_t arena = CallGuest(ctx, base, __imp__sub_83810258, {kSize, kGuestAlloc});
  if (!arena) {
    return false;
  }
  std::memset(ng2::guest::Virtual(arena), 0, kSize);
  g_arena = arena, g_arenaSize = kSize, g_arenaUsed = 0;
  g_entry = ArenaAlloc(kLabelRowSize);
  for (uint32_t& row : g_categoryRows) {
    row = ArenaAlloc(kLabelRowSize);
  }
  g_restoreRow = ArenaAlloc(kLabelRowSize);
  for (uint32_t& row : g_emptyRows) {
    row = ArenaAlloc(kLabelRowSize);
  }
  for (uint32_t& row : g_settingRows) {
    row = ArenaAlloc(kChoiceRowSize);
  }
  g_ids = ArenaAlloc(4 * kChoicesShown);
  g_entryText = GuestText(kEntryText);
  g_restoreText = GuestText(kRestoreText);
  g_emptyText = GuestText(u"");
  for (uint32_t i = 0; i < kPageCount; i++) {
    g_categoryText[i] = GuestText(kPages[i].name);
  }
  for (uint32_t i = 0; i < std::size(kSettings); i++) {
    const Setting& setting = kSettings[i];
    g_labelText[i] = GuestText(setting.label);
    if (IsListSetting(i)) {
      g_valueText[i] = GuestText(u"", kValueRoom);  // "< value >"
    } else {
      for (uint32_t k = 0; k < setting.count; k++) {
        g_choiceTexts[i][k] = GuestText(setting.choices[k]);
      }
    }
  }
  if (!g_valueText[5] || !g_labelText[std::size(kSettings) - 1]) {
    REXLOG_WARN("recomp settings: no room for the in-game rows");
    return false;
  }
  for (uint32_t k = 0; k < kChoicesShown; k++) {
    Store32(g_ids + 4 * k, kAnyText);
  }
  MakeLabelRow(ctx, base, g_entry);
  MakeLabelRow(ctx, base, g_restoreRow);
  for (uint32_t row : g_categoryRows) {
    MakeLabelRow(ctx, base, row);
  }
  for (uint32_t row : g_emptyRows) {
    MakeLabelRow(ctx, base, row);
  }
  for (uint32_t row : g_settingRows) {
    CallGuest(ctx, base, __imp__sub_8366E078, {row});
  }
  return true;
}

// A setting's row set up in a row slot of ours.
void SetUpSettingRow(PPCContext& ctx, uint8_t* base, uint32_t setting, uint32_t slot) {
  const Setting& that = kSettings[setting];
  uint32_t row = g_settingRows[setting];
  InitRow(ctx, base, row, slot);
  bool one = g_valueText[setting] != 0;
  CallGuest(ctx, base, __imp__sub_8366FCA0, {row, kAnyText, g_ids, one ? 1u : that.count, 0});
  Store32(row + kRowLabel, g_labelText[setting]);
  if (one) {
    SetGuestText(g_valueText[setting], ArrowText(that));
    Store32(row + kChoiceText, g_valueText[setting]);
  } else {
    for (uint32_t k = 0; k < that.count; k++) {
      Store32(row + kChoiceText + 0x100 * k, g_choiceTexts[setting][k]);
    }
  }
}

// Our rows set up again, as the game's builder does its own each time.
bool SetUpInGameRows(PPCContext& ctx, uint8_t* base) {
  if (!MakeInGameRowsOnce(ctx, base)) {
    return false;
  }
  InitRow(ctx, base, g_entry, kEntryIndex);
  Store32(g_entry + kRowLabel, g_entryText);
  for (uint32_t i = 0; i < kPageCount; i++) {
    InitRow(ctx, base, g_categoryRows[i], i);
    Store32(g_categoryRows[i] + kRowLabel, g_categoryText[i]);
  }
  return true;
}

// Restore Defaults after a page's rows, and the empty rows after it.
void SetUpPageEnd(PPCContext& ctx, uint8_t* base, uint32_t restoreSlot) {
  InitRow(ctx, base, g_restoreRow, restoreSlot);
  Store32(g_restoreRow + kRowLabel, g_restoreText);
  for (uint32_t i = 0; i < std::size(g_emptyRows); i++) {
    InitRow(ctx, base, g_emptyRows[i], restoreSlot + 1 + i, true);
    Store32(g_emptyRows[i] + kRowLabel, g_emptyText);
  }
}

bool g_inGameShownOrder = false;  // the game's rows placed with ours among them

void SendCursorEvent(PPCContext& ctx, uint8_t* base, uint32_t view) {
  CallGuest(ctx, base, __imp__sub_8366F158, {view, 2, kInGameModel});
}

// Tells the list's views the cursor moved (as the game's handler does).
void NotifyCursor(PPCContext& ctx, uint8_t* base, uint32_t cursor) {
  uint32_t last = ng2::guest::Load32(kInGameModel);
  if (last == cursor) {
    return;
  }
  Store32(kInGameModel + 4, last);
  Store32(kInGameModel, cursor);
  for (uint32_t view : kInGameViews) {
    SendCursorEvent(ctx, base, view);
  }
}

// The shown page's values into its rows (by their slots).
void PushValues(PPCContext& ctx, uint8_t* base) {
  const Page& page = kPages[g_category];
  for (uint32_t j = 0; j < page.count; j++) {
    uint32_t i = page.rows[j].setting;
    if (g_valueText[i]) {
      SetGuestText(g_valueText[i], ArrowText(kSettings[i]));
    }
    CallGuest(ctx, base, __imp__sub_8366E238,
              {kFirstPageRow + j, g_valueText[i] ? 0 : ValueOf(kSettings[i])});
  }
}

void SetListRows(const uint32_t* rows) {
  for (uint32_t i = 0; i < kGameRows; i++) {
    Store32(g_list + kRowsAt + 4 * i, rows[i]);
  }
}


// The game's rows in their own slots (other code finds Quit Game's by
// index), ours in the 13th.
void ArrangeGameRows() {
  SetListRows(g_gameRows);
  Store32(g_list + kRowsAt + 4 * kGameRows, g_entry);
}

// A row is drawn at the place its index (set when it's set up) gives it:
// ours and the two shown after it get those places.
void PlaceGameRows(PPCContext& ctx, uint8_t* base) {
  for (uint32_t shown = kEntryShownAt; shown <= kGameRows; shown++) {
    uint32_t index = ToGameIndex(shown);
    uint32_t row = index == kEntryIndex ? g_entry : g_gameRows[index];
    // Its texts, which the builder set after the setup: Quit Game's class
    // has its label at +0xF4 and its question's texts at +0x20..+0x28.
    bool quit = ng2::guest::Load32(row) == kQuitRowVtable;
    std::vector<uint32_t> keep = quit ? std::vector<uint32_t>{0x20, 0x24, 0x28, 0xF4}
                                      : std::vector<uint32_t>{kRowLabel};
    std::vector<uint32_t> values;
    for (uint32_t offset : keep) {
      values.push_back(ng2::guest::Load32(row + offset));
    }
    InitRow(ctx, base, row, shown);
    for (size_t i = 0; i < keep.size(); i++) {
      Store32(row + keep[i], values[i]);
    }
  }
}

}  // namespace

REX_EXTERN(__imp__sub_83672AF8);  // the list's event 1, to all its rows: (list, model)

namespace {

// A page row (in slot kFirstPageRow + line) moved to the right column, at
// that line from the top.
float PageRowShiftY() {
  return -float(kFirstPageRow) * LoadFloat(kLineHeight);
}

void ShiftSprite(uint32_t sprite, bool fromZero) {
  float x = fromZero ? 0.0f : LoadFloat(sprite + kSpriteOffset);
  float y = fromZero ? 0.0f : LoadFloat(sprite + kSpriteOffset + 4);
  StoreFloat(sprite + kSpriteOffset, x + kPageColumn);
  StoreFloat(sprite + kSpriteOffset + 4, y + PageRowShiftY());
}

// Right after the row's setup (for a choice row).
void PlacePageRow(uint32_t row, bool choices) {
  ShiftSprite(row + kRowSprite, true);
  if (choices) {
    uint32_t count = std::min<uint32_t>(ng2::guest::Load32(row + kChoiceCount), kChoicesShown);
    for (uint32_t k = 0; k < count; k++) {
      ShiftSprite(row + kChoiceItems + kChoiceItemSize * k, true);
      if (k + 1 < count) {
        ShiftSprite(row + kStrokes + kStrokeSize * k, false);
      }
    }
  }
}

// Our layout in the list, with a category's page (its rows set up again
// for the slots they're shown in); or the game's rows back. Moving between
// categories leaves the category rows as they are, as the title's list.
void ShowInGameRows(PPCContext& ctx, uint8_t* base, bool ours, uint32_t category = 0,
                    bool categoriesShown = false) {
  if (!ours) {
    ArrangeGameRows();
    g_inGameOurs = false;
    ng2::video::SetInGameRecompHeader(false);
    return;
  }
  g_category = category;
  const Page& page = kPages[category];
  uint32_t rows[kGameRows];
  for (uint32_t i = 0; i < kPageCount; i++) {
    rows[i] = g_categoryRows[i];
  }
  for (uint32_t j = 0; j < page.count; j++) {
    SetUpSettingRow(ctx, base, page.rows[j].setting, kFirstPageRow + j);
    rows[kFirstPageRow + j] = g_settingRows[page.rows[j].setting];
    PlacePageRow(rows[kFirstPageRow + j], true);
  }
  uint32_t restoreSlot = kFirstPageRow + page.count;
  SetUpPageEnd(ctx, base, restoreSlot);
  rows[restoreSlot] = g_restoreRow;
  PlacePageRow(g_restoreRow, false);
  for (uint32_t i = restoreSlot + 1; i < kGameRows; i++) {
    rows[i] = g_emptyRows[i - restoreSlot - 1];
  }
  SetListRows(rows);
  // The 13th stays: the move off our row still names index 12.
  g_inGameOurs = true;
  ng2::video::SetInGameRecompHeader(true);
  // Event 1 (as the list's rows get when it opens), to ours only: the
  // list's own sends it to its view too, which closes the menu.
  for (uint32_t i = categoriesShown ? kFirstPageRow : 0; i < kGameRows; i++) {
    CallGuest(ctx, base, __imp__sub_8366F158, {rows[i], 1, kInGameModel});
  }
  // A row shows a value once it changes: the slots may hold another
  // page's, so each is changed twice.
  for (uint32_t j = 0; j < page.count; j++) {
    CallGuest(ctx, base, __imp__sub_8366E238, {kFirstPageRow + j, 0xFF});
  }
  PushValues(ctx, base);
}

void LeaveInGameRows(PPCContext& ctx, uint8_t* base) {
  ShowInGameRows(ctx, base, false);
  Store32(kInGameCursors, kEntryIndex);
  NotifyCursor(ctx, base, kEntryIndex);
  if (g_windowChanged) {
    ScheduleWindow();
  }
  if (!g_written.empty() && !g_configPath.empty()) {
    SaveSettings();
  }
  g_written.clear();
}

}  // namespace

// The list's builder, run as it first opens: our row after its twelve.
REX_EXTERN(__imp__sub_836722A8);
REX_HOOK_RAW(sub_836722A8) {
  uint32_t list = ctx.r3.u32;
  __imp__sub_836722A8(ctx, base);
  if (g_inGameOurs && !g_written.empty() && !g_configPath.empty()) {
    SaveSettings();  // the list was made again while ours was shown
    g_written.clear();
  }
  g_inGameOurs = false;
  ng2::video::SetInGameRecompHeader(false);
  g_inGameShownOrder = false;
  if (SetUpInGameRows(ctx, base)) {
    g_list = list;
    for (uint32_t i = 0; i < kGameRows; i++) {
      g_gameRows[i] = ng2::guest::Load32(g_list + kRowsAt + 4 * i);
    }
    ArrangeGameRows();
    PlaceGameRows(ctx, base);
    g_inGameShownOrder = true;
  }
}

// A choice row's update: the choice mark of ours, placed again whenever the
// game places it, goes with the row; "< value >" rows have none, as the
// title's arrow lists.
REX_EXTERN(__imp__sub_8366F5A8);
REX_HOOK_RAW(sub_8366F5A8) {
  uint32_t row = ctx.r3.u32;
  uint32_t state = ng2::guest::Load32(row + 4);
  __imp__sub_8366F5A8(ctx, base);
  if (!g_inGameOurs || (state != 1 && state != 3)) {
    return;
  }
  for (uint32_t i = 0; i < std::size(kSettings); i++) {
    if (g_settingRows[i] == row) {
      if (g_valueText[i]) {
        StoreFloat(row + kChoiceMark + kSpriteOffset + 4, -10000.0f);
      } else {
        ShiftSprite(row + kChoiceMark, false);
      }
    }
  }
}

// The list's methods over its rows: ours too.
REX_EXTERN(__imp__sub_83672908);
REX_HOOK_RAW(sub_83672908) {  // reset
  uint32_t list = ctx.r3.u32;
  __imp__sub_83672908(ctx, base);
  if (list == g_list && !g_inGameOurs) {
    uint32_t row = ng2::guest::Load32(g_list + kRowsAt + 4 * kGameRows);
    CallAt(ctx, base, Method(row, 1), {row});
  }
}
REX_EXTERN(__imp__sub_83672960);
REX_HOOK_RAW(sub_83672960) {  // update
  uint32_t list = ctx.r3.u32;
  __imp__sub_83672960(ctx, base);
  if (list != g_list) {
    return;
  }
  if (!g_inGameOurs) {
    uint32_t row = ng2::guest::Load32(g_list + kRowsAt + 4 * kGameRows);
    CallAt(ctx, base, Method(row, 2), {row});
    return;
  }
  // The cursor mark at our row: a category, or a page row in the right
  // column (the list placed it by the heights of the rows above its slot).
  float origin = LoadFloat(kMarkOrigin), height = LoadFloat(kLineHeight);
  uint32_t line = g_inPage ? ng2::guest::Load32(kInGameCursors) - kFirstPageRow : g_category;
  StoreFloat(list + kCursorMark + kMarkX, origin + (g_inPage ? kPageColumn : 0.0f));
  StoreFloat(list + kCursorMark + kMarkY, std::floor(origin + float(line) * height));
}
REX_HOOK_RAW(sub_83672AF8) {  // event 1 to every row
  uint32_t list = ctx.r3.u32, model = ctx.r4.u32;
  __imp__sub_83672AF8(ctx, base);
  if (list == g_list && !g_inGameOurs) {
    uint32_t row = ng2::guest::Load32(g_list + kRowsAt + 4 * kGameRows);
    CallGuest(ctx, base, __imp__sub_8366F158, {row, 1, model});
  }
}
REX_EXTERN(__imp__sub_83672A40);
REX_HOOK_RAW(sub_83672A40) {  // draw: the rows one under another
  uint32_t list = ctx.r3.u32;
  uint64_t layer = ctx.r4.u64, position = ctx.r5.u64;
  __imp__sub_83672A40(ctx, base);
  if (list == g_list && !g_inGameOurs) {
    uint32_t bits = uint32_t(position);
    float y;
    std::memcpy(&y, &bits, sizeof(y));
    for (uint32_t i = 0; i < kGameRows; i++) {
      y += LoadFloat(ng2::guest::Load32(g_list + kRowsAt + 4 * i) + kRowHeight);
    }
    std::memcpy(&bits, &y, sizeof(bits));
    // x in the high word, y in the low one, as each row gets them.
    uint32_t row = ng2::guest::Load32(g_list + kRowsAt + 4 * kGameRows);
    CallAt(ctx, base, Method(row, 3), {row, layer, (position & 0xFFFFFFFF00000000ull) | bits});
  }
}

// Pad queries held back from the game's handler while ours acts.
REX_HOOK_RAW(sub_823E3448) {
  if (g_suppress & (1u << ctx.r7.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823E3448(ctx, base);
}
REX_HOOK_RAW(sub_823E3D78) {
  if (g_suppress & 0x10) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823E3D78(ctx, base);
}

// The list's handler: moves onto and off our row, and our settings while
// they're shown.
REX_EXTERN(__imp__sub_823E84F8);
REX_HOOK_RAW(sub_823E84F8) {
  using ng2::guest::Load32;
  uint32_t state = Load32(kInGameState);
  if (!g_list || state != 0) {
    __imp__sub_823E84F8(ctx, base);
    return;
  }
  uint32_t cursorAddress = kInGameCursors;
  uint32_t cursor = Load32(cursorAddress);
  uint32_t pad = Load32(kPadState);
  uint32_t player = *ng2::guest::Virtual(kPlayerPad);
  auto pressed = [&](uint32_t direction) {
    return CallGuest(ctx, base, __imp__sub_823E3448, {pad, player, 1, 0, direction}) != 0;
  };
  double sound = LoadFloat(kCursorSound);

  if (g_inGameOurs) {
    bool back = CallGuest(ctx, base, __imp__sub_823E3ED0, {kPadState, cursor, 0xFF, 0}) != 0;
    if (!g_inPage) {
      // The categories: up and down show one's page, Confirm or right go
      // into it, Back leaves.
      if (back) {
        CallGuest(ctx, base, __imp__sub_82429DA8, {2}, sound);
        LeaveInGameRows(ctx, base);
        return;
      }
      uint32_t category = g_category;
      if (pressed(0)) {
        category = (category + kPageCount - 1) % kPageCount;
      }
      if (pressed(1)) {
        category = (category + 1) % kPageCount;
      }
      if (category != g_category) {
        CallGuest(ctx, base, __imp__sub_82429DA8, {2}, sound);
        ShowInGameRows(ctx, base, true, category, true);
      }
      cursor = g_category;
      if (pressed(2) || CallGuest(ctx, base, __imp__sub_823E3D78, {kPadState, cursor, 0, 0})) {
        CallGuest(ctx, base, __imp__sub_82429DA8, {2}, sound);
        g_inPage = true;
        cursor = kFirstPageRow;
      }
    } else if (back) {
      CallGuest(ctx, base, __imp__sub_82429DA8, {2}, sound);
      g_inPage = false;
      cursor = g_category;
    } else {
      // A page: up and down pick a row, left and right change it, Y sets its
      // default, Confirm on Restore Defaults sets the page's.
      const Page& page = kPages[g_category];
      uint32_t rowCount = page.count + 1;
      uint32_t row = std::min(cursor - std::min(cursor, kFirstPageRow), rowCount - 1);
      uint32_t moved = row;
      if (pressed(0)) {
        moved = (moved + rowCount - 1) % rowCount;
      }
      if (pressed(1)) {
        moved = (moved + 1) % rowCount;
      }
      if (moved != row) {
        CallGuest(ctx, base, __imp__sub_82429DA8, {2}, sound);
        row = moved;
      }
      cursor = kFirstPageRow + row;
      if (row == page.count) {
        if (CallGuest(ctx, base, __imp__sub_823E3D78, {kPadState, cursor, 0, 0})) {
          CallGuest(ctx, base, __imp__sub_82429DA8, {3}, sound);
          for (uint32_t i = 0; i < page.count; i++) {
            RestoreDefault(kSettings[page.rows[i].setting]);
          }
        }
      } else {
        const Setting& setting = kSettings[page.rows[row].setting];
        uint32_t value = ValueOf(setting), changed = value;
        if (pressed(3)) {
          changed = (changed + setting.count - 1) % setting.count;
        }
        if (pressed(2)) {
          changed = (changed + 1) % setting.count;
        }
        if (changed != value) {
          CallGuest(ctx, base, __imp__sub_82429DA8, {3}, sound);
          SetValue(setting, changed);
        } else if (CallGuest(ctx, base, __imp__sub_823E3FD0, {kPadState})) {
          RestoreDefault(setting);
        }
      }
    }
    Store32(cursorAddress, cursor);
    if (g_windowChanged) {
      ScheduleWindow();
    }
    NotifyCursor(ctx, base, cursor);
    PushValues(ctx, base);
    return;
  }

  // The game's list, our row shown after Save Method: the moves onto and
  // off it are made here, in the order shown, the rest by the game's
  // handler in its own indices.
  bool up = pressed(0), down = pressed(1);
  uint32_t shown = ToShown(cursor), toShown = shown;
  if (shown == kEntryShownAt) {
    toShown = up ? kEntryShownAt - 1 : down ? kEntryShownAt + 1 : shown;
  } else if ((shown == kEntryShownAt - 1 && down) || (shown == kEntryShownAt + 1 && up)) {
    toShown = kEntryShownAt;
  }
  uint32_t to = ToGameIndex(toShown);
  if (to != cursor) {
    CallGuest(ctx, base, __imp__sub_82429DA8, {2}, sound);
    Store32(cursorAddress, to);
    cursor = to;
    g_suppress |= 0x3;  // the game's handler doesn't move it again
  }
  if (cursor == kEntryIndex) {
    g_suppress |= 0xC;  // no left and right either
    if (CallGuest(ctx, base, __imp__sub_823E3D78, {kPadState, cursor, 0, 0})) {
      CallGuest(ctx, base, __imp__sub_82429DA8, {2}, sound);
      g_inPage = false;
      ShowInGameRows(ctx, base, true, 0);
      Store32(cursorAddress, 0);
      NotifyCursor(ctx, base, 0);
      g_suppress = 0;
      return;
    }
    g_suppress |= 0x10;  // the game's own Confirm reads past its rows
  }
  __imp__sub_823E84F8(ctx, base);
  g_suppress = 0;
}


// The list's cursor event, as the game's (sub_83672BE8) but for rows
// placed in the order shown: the rows to light and unlight are found by
// the handler's indices, and they compare the cursor with their own index
// (their place), so they get the cursors in the order shown; the cursor
// mark's slot (+0x4EC0) is the shown place too.
REX_EXTERN(__imp__sub_83672BE8);
REX_HOOK_RAW(sub_83672BE8) {
  uint32_t list = ctx.r3.u32, model = ctx.r4.u32;
  if (list != g_list || !g_inGameShownOrder || g_inGameOurs) {
    __imp__sub_83672BE8(ctx, base);
    return;
  }
  using ng2::guest::Load32;
  uint32_t cursor = Load32(model), last = Load32(model + 4);
  Store32(model, ToShown(cursor));
  Store32(model + 4, int32_t(last) >= 0 ? ToShown(last) : last);
  if (int32_t(last) >= 0) {
    CallGuest(ctx, base, __imp__sub_8366F158,
              {Load32(list + kRowsAt + 4 * last), 2, model});
  }
  CallGuest(ctx, base, __imp__sub_8366F158, {Load32(list + kRowsAt + 4 * cursor), 2, model});
  Store32(model, cursor);
  Store32(model + 4, last);
  Store32(list + kCursorMarkSlot, ToShown(cursor));
  ctx.r3.u64 = 1;
}
