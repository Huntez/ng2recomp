// ng2 - Ninja Gaiden II static recompilation
//
// Downloadable content: the packages in the game folder's dlc/ installed
// into the user data folder, where the game finds them.

#pragma once

#include <filesystem>

namespace rex::system {
class KernelState;
}

namespace ng2::dlc {

// This game's downloadable content: an STFS package (LIVE, PIRS or CON) of
// content type 2 for title 544307D5.
bool IsGameDlc(const std::filesystem::path& path);

// Installs every content package (STFS LIVE/PIRS/CON file) under
// <game_root>/dlc that isn't installed yet. A marker per package in
// <user_root>/dlc_installed records the size and time of the installed copy,
// so a replaced package is installed again.
void InstallPackages(rex::system::KernelState* kernel_state,
                     const std::filesystem::path& game_root,
                     const std::filesystem::path& user_root);

}  // namespace ng2::dlc
