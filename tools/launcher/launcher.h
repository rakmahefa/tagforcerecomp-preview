// launcher.h - shared declarations of the Linux launcher.
//
// Tag Force Recomp (Linux launcher): a window that takes the player's disc
// image (Yu-Gi-Oh! GX Tag Force, ULES-00600), checks the tools, prepares the
// game files and the translated code, and starts the game. Five pages on the
// left - Your disc, Tools, Options, Build, Play - like the DQ8Recomp launcher's
// flow, drawn over a night-blue background with gold accents.
//
// This code is original (MIT, like the rest of the repository); only the
// disc-detection logic mirrors the project's own runtime/core/gameid.cpp.
#pragma once

#define IMGUI_DEFINE_MATH_OPERATORS // ImVec2 + ImVec2 and friends
#include <imgui.h>
#include <SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tf::launcher {

// ---------------------------------------------------------------- constants

// The one release this preview supports.
constexpr const char *kGameId = "ULES-00600";
constexpr const char *kGameTitle = "Yu-Gi-Oh! GX Tag Force";

// Window size, in the spirit of the reference launcher.
constexpr int kWindowWidth = 1180, kWindowHeight = 780;

// The five pages, in the order the player walks them.
enum class Page : uint8_t { Disc, Tools, Options, Build, Play, Count };
extern const char *const kPageNames[static_cast<size_t>(Page::Count)];

// Base font size everything else is measured in (em).
constexpr float kBaseFont = 18.0f;
inline float em(float x) { return x * kBaseFont; }

// ---------------------------------------------------------------- palette

namespace palette {
// Night blue + gold, matching the reference launcher's mood.
constexpr ImU32 kWindow = IM_COL32(13, 17, 40, 244);
constexpr ImU32 kGold = IM_COL32(240, 200, 92, 255);
constexpr ImU32 kGoldSoft = IM_COL32(222, 178, 66, 255);
constexpr ImU32 kGoldDeep = IM_COL32(198, 148, 40, 255);
constexpr ImU32 kText = IM_COL32(233, 237, 248, 255);
constexpr ImU32 kTextMuted = IM_COL32(146, 155, 182, 255);
constexpr ImU32 kGood = IM_COL32(112, 198, 126, 255);
constexpr ImU32 kBad = IM_COL32(229, 111, 111, 255);
constexpr ImU32 kWarn = IM_COL32(235, 177, 92, 255);
constexpr ImU32 kFill = IM_COL32(19, 25, 52, 255);
constexpr ImU32 kFillStrong = IM_COL32(27, 33, 63, 255);
constexpr ImU32 kBorder = IM_COL32(62, 72, 110, 160);
constexpr ImU32 kBorderInner = IM_COL32(46, 55, 90, 200);
inline ImVec4 rgba(ImU32 packed) { return ImGui::ColorConvertU32ToFloat4(packed); }
inline ImU32 withAlpha(ImU32 c, int alpha) {
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(static_cast<unsigned>(alpha) & 0xFFu) << IM_COL32_A_SHIFT);
}
} // namespace palette

// ---------------------------------------------------------------- fonts

// Loaded by launcher_main.cpp; nullptr entries fall back to the default font.
struct Fonts {
    ImFont *regular = nullptr; // 18 px body
    ImFont *bold = nullptr;    // 18 px emphasis
    ImFont *bold22 = nullptr;  // card titles
    ImFont *bold24 = nullptr;  // the build's percentage
    ImFont *bold28 = nullptr;  // page titles
    ImFont *bold34 = nullptr;  // "Ready to play"
};
extern Fonts g_fonts;
void pushFont(const ImFont *font); // ImGui::PushFont, no-op for nullptr
void popFont();

// ---------------------------------------------------------------- helpers

std::string humanSize(uint64_t bytes); // "1.06 GB"
std::string duration(double seconds);  // "42 s", "6 min", "1 h 05 min"
std::string shellQuote(const std::string &path);

// ---------------------------------------------------------------- config

// What the launcher remembers between runs (a small ini beside nothing else).
struct LauncherConfig {
    std::string disc;
    std::string workspace;
    int jobs = 0; // 0 picks from the machine
};
std::filesystem::path configPath(); // .../TagForceRecomp/Launcher/launcher.ini
LauncherConfig loadConfig();
void saveConfig(const LauncherConfig &config);
int defaultJobs();                // one a core, within memory
std::string defaultGamesFolder(); // ~/TagForceRecomp

// ---------------------------------------------------------------- the disc

struct DiscInfo {
    bool readable = false;
    std::string volume; // the ISO's volume label
    std::string gameId; // ULES-00600 &c, from UMD_DATA.BIN
    uint64_t size = 0;
    bool supported = false;
    std::string error; // what to tell the player when !supported
};
DiscInfo inspectDisc(const std::filesystem::path &iso);

// ---------------------------------------------------------------- the tools

struct ToolCheck {
    std::string name, purpose, version, problem;
    bool found = false;
    bool optional = false;
    bool usable() const { return found && problem.empty(); }
};
struct ToolReport {
    std::vector<ToolCheck> tools;
    std::string installCommand; // a copyable line for the terminal
    std::string instructions;
    bool ready() const;
};
ToolReport checkTools(); // runs `timeout 5 <tool> --version` for each

// ---------------------------------------------------------------- the build

enum class Stage : uint8_t { CheckDisc, UnpackSource, CopyFiles, TranslateCode, CompileRuntime, Count };
constexpr size_t kStageCount = static_cast<size_t>(Stage::Count);
const char *stageTitle(Stage stage);

enum class StageStatus : uint8_t { Waiting, Running, Done, Failed, Skipped };
inline const char *statusName(StageStatus status) {
    switch (status) {
    case StageStatus::Waiting: return "waiting";
    case StageStatus::Running: return "running";
    case StageStatus::Done: return "done";
    case StageStatus::Failed: return "failed";
    case StageStatus::Skipped: return "skipped";
    }
    return "?";
}
struct StageState {
    StageStatus status = StageStatus::Waiting;
    double progress = -1.0;   // < 0: unknown, the bar sweeps
    double seconds = 0.0;     // how long it took / has been running
    double startedAt = -1.0;  // set when the stage begins
    double remaining = -1.0;
    std::string detail, note;
};

// ---------------------------------------------------------------- the release

// A release carries its own source next to the launcher, in payload/.
struct Payload {
    std::string version;
    std::filesystem::path source; // payload/source
};
std::optional<Payload> findPayload(); // next to the executable

} // namespace tf::launcher
