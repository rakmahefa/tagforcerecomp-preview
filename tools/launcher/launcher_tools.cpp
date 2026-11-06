// launcher_tools.cpp - looks for the tools and libraries the Linux build needs.
//
// On Linux the Tools page checks what tools/build_linux.sh compiles and links against -
// g++, Python 3, SDL2, the FFmpeg/PNG/JPEG libraries, Vulkan - and shows the
// distribution's install command to copy. Two former build-time dependencies are
// no longer the player's problem, because the release payload carries them in
// source/prebuilt/: the Vulkan shaders (gpu_spv.h, so glslc is optional) and
// TinyCC 0.9.27 (libtcc.so, built at package time; a git checkout compiles it
// from the official source on the first Build instead).
#include "launcher_tools.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

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

// The version `pkg-config --modversion module` prints (the -dev package's library version).
std::string modVersion(const char *module) {
    const auto [code, line] = probe(std::string("timeout 5 pkg-config --modversion ") + module + " 2>/dev/null");
    if (code != 0 || line.empty())
        return {};
    return line;
}

// "found" when any of the files exists, empty when none does.
std::string presentAt(const char *files) {
    const auto [code, line] = probe(std::string("timeout 5 sh -c 'ls ") + files + " 2>/dev/null | head -1'");
    if (code != 0 || line.empty())
        return {};
    return "found";
}

} // namespace

bool ToolReport::ready() const {
    for (const ToolCheck &tool : tools)
        if (!tool.optional && !tool.usable())
            return false;
    return true;
}

ToolReport checkTools(const std::filesystem::path &source) {
    ToolReport report;
    const auto add = [&](const char *name, const char *purpose, const std::string &version, bool optional = false,
                         const char *problemWhenMissing = nullptr) {
        ToolCheck check;
        check.name = name;
        check.purpose = purpose;
        check.version = version;
        check.found = !version.empty();
        check.optional = optional;
        if (!check.found)
            check.problem = "Not found. " +
                            std::string(problemWhenMissing ? problemWhenMissing
                                                           : (optional ? "The game works without it."
                                                                       : "Install it and press Check again."));
        report.tools.push_back(check);
    };
    add("C++ compiler", "Compiles the game", versionOf("g++"));
    add("Python 3", "Copies the disc and translates the game code", versionOf("python3"));
    // the Vulkan shaders ship pre-compiled (prebuilt/gpu_spv.h): glslc is only for recompiling them
    if (std::filesystem::is_regular_file(source / "prebuilt" / "gpu_spv.h"))
        add("Shader compiler", "Recompiles the Vulkan shaders (they ship pre-compiled)", "bundled (prebuilt/gpu_spv.h)",
            true, "Not needed to build: the release carries the Vulkan shaders pre-compiled.");
    else
        add("Shader compiler", "Compiles the Vulkan shaders (glslc; this checkout has none pre-compiled)", versionOf("glslc"),
            true, "A release payload carries the Vulkan shaders pre-compiled; a checkout without prebuilt/gpu_spv.h needs glslc.");
    add("SDL2", "Window, input and audio", modVersion("sdl2"));
    add("FFmpeg libraries", "Play the movies and music", modVersion("libavcodec"));
    add("PNG/JPEG codecs", "Read images and textures", modVersion("libpng"));
    add("Vulkan", "Draws the game (or the software renderer)", modVersion("vulkan"));
    // TinyCC rides in the payload's prebuilt/tcc; only a plain git checkout builds it
    add("TinyCC", "Compiles the translated game code at run time",
        std::filesystem::is_regular_file(source / "prebuilt" / "tcc" / "libtcc.so")
            ? "bundled (prebuilt/tcc, 0.9.27)"
            : presentAt("/usr/lib/x86_64-linux-gnu/libtcc.so /usr/lib64/libtcc.so /usr/local/lib/libtcc.so"),
        true,
        "The release payload carries TinyCC pre-compiled; a plain git checkout compiles it from "
        "the official source on the first Build (needs make, and curl or wget).");
    if (!report.ready()) {
        report.installCommand =
            "sudo apt install build-essential python3 pkg-config bzip2 libsdl2-dev libavcodec-dev "
            "libavformat-dev libavutil-dev libswscale-dev libswresample-dev libpng-dev libjpeg-dev libvulkan-dev";
        report.instructions =
            "Your distribution's packages have them all. Copy the line below into a terminal, enter your password, "
            "and press Check again when it finishes. (On Fedora: sudo dnf install gcc-c++ python3 pkgconf-pkg-config "
            "bzip2 SDL2-devel ffmpeg-devel libpng-devel libjpeg-devel vulkan-headers vulkan-loader-devel.)";
    }
    return report;
}

} // namespace tf::launcher
