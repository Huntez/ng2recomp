// ng2 - Ninja Gaiden II static recompilation
//
// Movies: the game's XMedia2 player replaced by host decoding
// (movie_hooks.cpp, host_movie.cpp).

#pragma once

#include <filesystem>

namespace ng2::movie {

// Where "game:\" movie paths point.
void SetGameDataRoot(const std::filesystem::path& root);

}  // namespace ng2::movie
