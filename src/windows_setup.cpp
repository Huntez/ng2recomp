// ng2 - Ninja Gaiden II static recompilation

#include "windows_setup.h"

#ifdef _WIN32

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <SDL3/SDL.h>
#include <rex/cvar.h>
#include <rex/rex_app.h>

namespace ng2::windows_setup {
namespace fs = std::filesystem;

namespace {

constexpr const char* kTitle = "Ninja Gaiden II";
// Title Update 2's default.xexp (ng2_app.h checks the same size).
constexpr uintmax_t kTitleUpdate2Size = 2318336;

// UTF-8 for SDL and the settings (path::string() is the ANSI code page on
// Windows, and throws for characters it can't hold).
std::string Utf8(const fs::path& path, bool generic = false) {
  std::u8string text = generic ? path.generic_u8string() : path.u8string();
  return std::string(text.begin(), text.end());
}

void Message(const std::string& text, bool error) {
  SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, kTitle,
                           text.c_str(), nullptr);
}

fs::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  fs::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
    result = path;
  }
  CoTaskMemFree(path);
  return result;
}

// The shell's file dialog: a folder, or a file; empty if cancelled.
fs::path Pick(bool folder, const wchar_t* title) {
  fs::path result;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) {
    return result;
  }
  FILEOPENDIALOGOPTIONS options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
  dialog->SetTitle(title);
  IShellItem* item = nullptr;
  if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item))) {
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
      result = path;
    }
    CoTaskMemFree(path);
    item->Release();
  }
  dialog->Release();
  return result;
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

// One file from an STFS package (LIVE, PIRS, CON) at its root, as
// tools/stfs_extract.py does; empty if it isn't there.
std::vector<uint8_t> ExtractFromPackage(const std::vector<uint8_t>& d, const std::string& wanted) {
  constexpr uint32_t kBlock = 0x1000, kPerTable = 0xAA, kPerL1 = 0x70E4;
  if (d.size() < 0x3A0 || (std::memcmp(d.data(), "LIVE", 4) && std::memcmp(d.data(), "PIRS", 4) &&
                           std::memcmp(d.data(), "CON ", 4))) {
    return {};
  }
  auto be32 = [&](size_t at) {
    return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
  };
  uint32_t headerSize = be32(0x340);
  const uint8_t* vd = d.data() + 0x379;
  uint32_t shift = (vd[2] & 1) ? 0 : 1;
  uint32_t tableBlocks = uint32_t(vd[3]) | uint32_t(vd[4]) << 8;
  uint32_t tableStart = uint32_t(vd[5]) | uint32_t(vd[6]) << 8 | uint32_t(vd[7]) << 16;
  uint64_t first = (uint64_t(headerSize) + 0xFFF) & ~uint64_t(0xFFF);
  auto backing = [&](uint32_t b) -> uint64_t {
    uint64_t r = (uint64_t((b + kPerTable) / kPerTable) << shift) + b;
    if (b < kPerTable) {
      return r;
    }
    if (b < kPerL1) {
      return r + (uint64_t((b + kPerL1) / kPerL1) << shift);
    }
    return (uint64_t(1) << shift) + r + (uint64_t((b + kPerL1) / kPerL1) << shift);
  };
  auto offset = [&](uint32_t b) { return first + backing(b) * kBlock; };
  auto hashTable = [&](uint32_t b) {
    uint64_t n = uint64_t(b / kPerTable) * (kPerTable + (1u << shift));
    if (b >= kPerTable) {
      n += uint64_t(b / kPerL1 + 1) << shift;
    }
    if (b >= kPerL1) {
      n += uint64_t(1) << shift;
    }
    return first + n * kBlock;
  };
  auto nextBlock = [&](uint32_t b) -> uint32_t {
    uint64_t e = hashTable(b) + (b % kPerTable) * 0x18;
    if (e + 0x18 > d.size()) {
      return 0xFFFFFF;
    }
    return uint32_t(d[e + 0x15]) << 16 | uint32_t(d[e + 0x16]) << 8 | d[e + 0x17];
  };
  uint32_t b = tableStart;
  for (uint32_t t = 0; t < tableBlocks; t++) {
    uint64_t table = offset(b);
    if (table + kBlock > d.size()) {
      return {};
    }
    for (uint32_t i = 0; i < kBlock / 0x40; i++) {
      const uint8_t* e = d.data() + table + i * 0x40;
      uint8_t flags = e[0x28];
      std::string name(reinterpret_cast<const char*>(e), flags & 0x3F);
      bool atRoot = e[0x32] == 0xFF && e[0x33] == 0xFF;
      if (name.empty() || (flags & 0x80) || !atRoot || _stricmp(name.c_str(), wanted.c_str()) != 0) {
        continue;
      }
      if (shift && !(flags & 0x40)) {
        return {};  // fragmented in a two-table package: not supported
      }
      uint32_t size = uint32_t(e[0x34]) << 24 | uint32_t(e[0x35]) << 16 | uint32_t(e[0x36]) << 8 | e[0x37];
      if (size > d.size()) {
        return {};  // can't be in this package
      }
      uint32_t block = uint32_t(e[0x2F]) | uint32_t(e[0x30]) << 8 | uint32_t(e[0x31]) << 16;
      std::vector<uint8_t> data;
      while (data.size() < size) {
        uint64_t at = offset(block);
        if (at >= d.size()) {
          return {};  // truncated
        }
        size_t n = size_t(std::min<uint64_t>(kBlock, d.size() - at));
        data.insert(data.end(), d.begin() + at, d.begin() + at + n);
        block = (flags & 0x40) ? block + 1 : nextBlock(block);
      }
      data.resize(size);
      return data;
    }
    if (shift && tableBlocks > 1) {
      return {};  // a file table over blocks of a two-table package: not supported
    }
    b = nextBlock(b);
  }
  return {};
}

bool IsGameFolder(const fs::path& dir) {
  std::error_code ec;
  return !dir.empty() && fs::is_regular_file(dir / "default.xex", ec);
}

// Title Update 2 next to default.xex: asked for once (its package, or an
// extracted default.xexp) and written there.
bool EnsureTitleUpdate(const fs::path& game) {
  std::error_code ec;
  if (fs::file_size(game / "default.xexp", ec) == kTitleUpdate2Size) {
    return true;
  }
  Message("This port runs the game with Title Update 2. Choose the update: its package "
          "(TU_1A461UL_000000G000000.00000000000G4) or an extracted default.xexp.",
          false);
  fs::path update = Pick(false, L"Title Update 2 (package or default.xexp)");
  if (update.empty()) {
    return false;
  }
  constexpr uintmax_t kMaxPackage = 64ull << 20;  // the update is a few MB
  if (fs::file_size(update, ec) > kMaxPackage || ec) {
    Message("That isn't Title Update 2 for this game.", true);
    return false;
  }
  std::vector<uint8_t> file = ReadFile(update);
  std::vector<uint8_t> xexp = file.size() >= 4 && std::memcmp(file.data(), "XEX2", 4) == 0
                                  ? file
                                  : ExtractFromPackage(file, "default.xexp");
  if (xexp.size() != kTitleUpdate2Size || std::memcmp(xexp.data(), "XEX2", 4) != 0) {
    Message("That isn't Title Update 2 for this game.", true);
    return false;
  }
  fs::path part = game / "default.xexp.part";
  bool written = false;
  {
    std::ofstream out(part, std::ios::binary);
    written = out.write(reinterpret_cast<const char*>(xexp.data()), std::streamsize(xexp.size())) &&
              out.flush();
  }
  if (written) {
    fs::rename(part, game / "default.xexp", ec);
  }
  if (!written || ec) {
    fs::remove(part, ec);
    Message("Can't write to " + Utf8(game) + ": copy default.xexp there yourself.", true);
    return false;
  }
  return true;
}

}  // namespace

bool ConfigurePaths(rex::PathConfig& paths) {
  fs::path data = KnownFolder(FOLDERID_LocalAppData) / "NinjaGaiden2";
  std::error_code ec;
  fs::create_directories(data / "user", ec);

  // Settings, saves and the log in %LOCALAPPDATA% (the program's folder may
  // not be writable), unless given.
  const char* config = std::getenv("NG2_CONFIG");
  if (!config || !*config) {
    paths.config_path = data / "ng2.toml";
  }
  auto given = [](const char* name) {
    const auto* info = rex::cvar::GetFlagInfo(name);
    return info && info->source != rex::cvar::Source::kDefault;
  };
  if (!given("user_data_root")) {
    paths.user_data_root = data / "user";
  }
  if (!given("cache_root")) {
    paths.cache_root = data / "cache";
  }
  const auto* logFile = rex::cvar::GetFlagInfo("log_file");
  if (logFile && logFile->source == rex::cvar::Source::kDefault) {
    // Forward slashes: the settings file is TOML, where a backslash escapes.
    rex::cvar::SetFlagByName("log_file", Utf8(data / "ng2.log", true));
  }

  // The game folder: given, remembered, or asked for.
  fs::path remembered = data / "game_dir.txt";
  if (!given("game_data_root")) {
    std::ifstream in(remembered);
    std::string line;
    if (std::getline(in, line)) {
      paths.game_data_root = fs::path(std::u8string(line.begin(), line.end()));
    }
  }
  if (!IsGameFolder(paths.game_data_root)) {
    Message("Choose the folder with your copy of the game (the one with default.xex).", false);
    fs::path chosen = Pick(true, L"The game's folder (with default.xex)");
    if (chosen.empty()) {
      return false;
    }
    if (!IsGameFolder(chosen)) {
      Message("There's no default.xex in " + Utf8(chosen) + ".", true);
      return false;
    }
    paths.game_data_root = chosen;
    std::ofstream(remembered) << Utf8(chosen) << '\n';
  }
  return EnsureTitleUpdate(paths.game_data_root);
}

}  // namespace ng2::windows_setup

#endif  // _WIN32
