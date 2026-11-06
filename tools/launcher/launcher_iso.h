// launcher_iso.h - recognises the game's disc image.
#pragma once

#include "launcher.h"

namespace tf::launcher {

// Reads the ISO9660 primary volume descriptor, finds UMD_DATA.BIN in the root
// directory and takes the game id from it (ULES-00600 for the European
// Yu-Gi-Oh! GX Tag Force) - the same logic as runtime/core/gameid.cpp.
DiscInfo inspectDisc(const std::filesystem::path &iso);

} // namespace tf::launcher
