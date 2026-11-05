// ng2 - Ninja Gaiden II static recompilation

#include "dlc.h"

#include <cstring>
#include <fstream>
#include <string>
#include <system_error>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>

namespace ng2::dlc {
namespace fs = std::filesystem;
using rex::X_RESULT;

namespace {

constexpr uint32_t kTitleId = 0x544307D5;
constexpr uint32_t kMarketplaceContent = 2;

uint32_t BigEndian32(const unsigned char* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Where InstallContent puts a package (and whether it is still there).
bool Installed(rex::system::xam::ContentManager* content, const fs::path& package) {
  rex::system::xam::XCONTENT_AGGREGATE_DATA data;
  data.device_id = static_cast<uint32_t>(rex::system::xam::DummyDeviceId::HDD);
  data.content_type = rex::system::XContentType::kMarketplaceContent;
  data.title_id = kTitleId;
  data.xuid = 0;
  data.set_file_name(rex::path_to_utf8(package.filename()));
  return content->ContentExists(0, data);
}

std::string Stamp(const fs::directory_entry& entry) {
  std::error_code ec;
  auto size = entry.file_size(ec);
  auto time = entry.last_write_time(ec).time_since_epoch().count();
  return std::to_string(size) + " " + std::to_string(time);
}

std::string ReadMarker(const fs::path& path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return line;
}

}  // namespace

// A game, title update or save copied in with the packages is not DLC.
bool IsGameDlc(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  unsigned char header[0x364] = {};
  if (!file.read(reinterpret_cast<char*>(header), sizeof(header))) {
    return false;
  }
  bool stfs = !std::memcmp(header, "LIVE", 4) || !std::memcmp(header, "PIRS", 4) ||
              !std::memcmp(header, "CON ", 4);
  return stfs && BigEndian32(header + 0x344) == kMarketplaceContent &&
         BigEndian32(header + 0x360) == kTitleId;
}

void InstallPackages(rex::system::KernelState* kernel_state, const fs::path& game_root,
                     const fs::path& user_root) {
  const fs::path dir = game_root / "dlc";
  std::error_code ec;
  if (!kernel_state || !fs::is_directory(dir, ec)) {
    return;
  }
  const fs::path markers = user_root / "dlc_installed";
  fs::create_directories(markers, ec);

  // Package folders as the console keeps them (544307D5/00000002/<id>) work
  // too, so the files are found at any depth.
  auto* content = kernel_state->content_manager();
  for (auto it = fs::recursive_directory_iterator(
           dir, fs::directory_options::follow_directory_symlink, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const auto& entry = *it;
    if (!entry.is_regular_file(ec) || !IsGameDlc(entry.path())) {
      continue;
    }
    const fs::path marker = markers / entry.path().filename();
    const std::string stamp = Stamp(entry);
    if (ReadMarker(marker) == stamp && Installed(content, entry.path())) {
      continue;
    }
    auto result = content->InstallContent(entry.path());
    if (result != X_ERROR_SUCCESS) {
      REXLOG_ERROR("DLC: cannot install {} ({:08X})", rex::path_to_utf8(entry.path()), result);
      fs::remove(marker, ec);  // tried again next time, over the partial copy
      continue;
    }
    std::ofstream out(marker);
    if (!(out << stamp << '\n')) {
      REXLOG_WARN("DLC: cannot write {}; it will be installed again", rex::path_to_utf8(marker));
    }
    REXLOG_INFO("DLC: installed {}", rex::path_to_utf8(entry.path().filename()));
  }
}

}  // namespace ng2::dlc
