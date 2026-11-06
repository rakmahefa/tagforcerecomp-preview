// launcher_tools.h - checks the tools the game build needs.
#pragma once

#include "launcher.h"

namespace tf::launcher {

// Runs `<tool> --version` (under `timeout`) for each tool the pipeline needs
// and reports what was found. `source` is the payload's source/ (or the
// checkout), where the prebuilt/ shaders and TinyCC ride. Slow enough to run
// on a worker thread.
ToolReport checkTools(const std::filesystem::path &source);

} // namespace tf::launcher
