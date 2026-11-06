// launcher_png.h - writes the launcher's screenshots as PNG.
#pragma once

#include <cstdint>
#include <string>

namespace tf::launcher {

// Saves width x height RGBA pixels (a row is width*4 bytes, the first row at
// the top) as a PNG. Pure stored (uncompressed) deflate blocks: no zlib
// dependency, files stay small enough for documentation and pull requests.
bool savePng(const std::string &path, int width, int height, const uint8_t *rgba);

} // namespace tf::launcher
