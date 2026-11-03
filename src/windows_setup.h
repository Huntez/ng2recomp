// ng2 - Ninja Gaiden II static recompilation
//
// Windows first-run setup (windows_setup.cpp): what the AppImage's launcher
// does on Linux.

#pragma once

#include <filesystem>

namespace rex {
struct PathConfig;
}

namespace ng2::windows_setup {

// Fills in what the command line left out: the game folder (remembered, or
// asked for once, with Title Update 3's default.xexp extracted into it from
// its package), and the settings, saves and log in %LOCALAPPDATA%. False if
// the player cancelled or the game can't start.
bool ConfigurePaths(rex::PathConfig& paths);

}  // namespace ng2::windows_setup
