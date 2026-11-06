// launcher_tools.cpp - looks for the tools the Linux build pipeline needs.
//
// Nothing is bundled yet (the Windows release carries its own Python and
// TinyCC): on Linux the Tools page checks g++, CMake, Ninja, Python 3 and
// FFmpeg, and shows the distribution's install command to copy.
#include "launcher_tools.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tf::launcher {
namespace {

// Runs a short command and returns (exit code, first line of output).
std::pair<int, std::string> probe(const std::string &command) {
    FILE *pipe = popen(command.c_str(), "r"); // the launcher's own UTF-8 locale
    if (!pipe)
        return {-1, {}};
    std::string line;
    int c;
    while ((c = fgetc(pipe)) != EOF && c != '\n' && c != '\r' && line.size() < 200)
        line += static_cast<char>(c);
    const int code = pclose(pipe);
    return {code, line};
}

// The version line of `tool --version`, or empty when the tool is missing.
std::string versionOf(const char *tool, const char *flag = "--version") {
    // 5 s: a present tool answers at once; a hung one must not freeze the page.
    const std::string command = std::string("timeout 5 ") + tool + " " + flag + " 2>/dev/null";
    const auto [code, line] = probe(command);
    if (code != 0 || line.empty())
        return {};
    return line;
}

} // namespace

bool ToolReport::ready() const {
    for (const ToolCheck &tool : tools)
        if (!tool.optional && !tool.usable())
            return false;
    return true;
}

ToolReport checkTools() {
    ToolReport report;
    const auto add = [&](const char *name, const char *purpose, const std::string &version, bool optional = false) {
        ToolCheck check;
        check.name = name;
        check.purpose = purpose;
        check.version = version;
        check.found = !version.empty();
        check.optional = optional;
        report.tools.push_back(check);
    };
    add("C++ compiler", "Compiles the game", versionOf("g++"));
    add("CMake", "Configures the runtime build", versionOf("cmake"));
    add("Ninja", "Runs the build", versionOf("ninja"));
    add("Python 3", "Copies the disc and translates the game code", versionOf("python3"));
    add("FFmpeg", "Plays the movies and music; without it they stay silent", versionOf("ffmpeg", "-version"), true);
    // Wait for a missing optional tool to explain itself on the page.
    for (ToolCheck &tool : report.tools)
        if (!tool.found)
            tool.problem = "Not found. " + std::string(tool.optional ? "The game works without it." : "Install it and press Check again.");
    if (!report.ready()) {
        report.installCommand = "sudo apt install build-essential cmake ninja-build python3 ffmpeg";
        report.instructions =
            "Your distribution's packages have them all. Copy the line below into a terminal, enter your password, "
            "and press Check again when it finishes. (On Fedora the names are gcc-c++ cmake ninja-build python3 ffmpeg.)";
    }
    return report;
}

} // namespace tf::launcher
