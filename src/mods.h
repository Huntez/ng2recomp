// ng2 - Ninja Gaiden II static recompilation
//
// Mods: game files replaced by the files in a mods folder (src/mods.cpp).

#pragma once

#include <filesystem>

namespace ng2::mods {

// Reads the mods in <folder> and, if any is on, makes the game load their
// files in place of its own. Called once the game's image is loaded and
// before it runs; game_root holds the game's archives (for [replace]),
// cache_dir takes the game files [replace] copies out of them.
void Activate(const std::filesystem::path& folder, const std::filesystem::path& game_root,
              const std::filesystem::path& cache_dir);

}  // namespace ng2::mods
