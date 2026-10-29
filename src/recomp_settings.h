// ng2 - Ninja Gaiden II static recompilation
//
// RECOMP SETTINGS in the game's Options screen (recomp_settings.cpp).

#pragma once

#include <filesystem>

namespace rex::ui {
class WindowedAppContext;
}

namespace ng2::recomp_settings {

// Where the settings are saved when the menu closes (the runtime's config).
void SetConfigPath(const std::filesystem::path& path);
// Where the window's settings are applied (its UI thread).
void SetAppContext(rex::ui::WindowedAppContext* context);

}  // namespace ng2::recomp_settings
