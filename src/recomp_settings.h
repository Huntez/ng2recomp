// ng2 - Ninja Gaiden II static recompilation
//
// RECOMP SETTINGS in the game's Options screen (recomp_settings.cpp).

#pragma once

#include <filesystem>

namespace ng2::recomp_settings {

// Where the settings are saved when the menu closes (the runtime's config).
void SetConfigPath(const std::filesystem::path& path);

}  // namespace ng2::recomp_settings
