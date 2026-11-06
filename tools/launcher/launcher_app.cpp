// launcher_app.cpp - the launcher's window, from the player's disc to Play.
//
// Five pages in a sidebar over a night sky, gold for what matters, in the
// spirit of the DQ8Recomp launcher (this file is original code; see
// launcher.h for the license note). The pages do real work: the disc is read
// and recognised, the tools are probed, the build runs as child processes,
// and Play starts the game once a Linux runtime lands beside the launcher.
#include "launcher_app.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace tf::launcher {
namespace {

// How much of the whole build each step is, for the overall bar.
constexpr double kStageWeight[kStageCount] = {0.02, 0.03, 0.30, 0.55, 0.10};

constexpr double kDiscPreviewSize = 1073741824.0; // 1.07 GB, for the screenshots

// ---------------------------------------------------------------- icons

enum class Icon : uint8_t {
    Disc, Wrench, Sliders, Hammer, Play, Check, Close, Warning, Folder, Copy, Info, Star, Link, Reset
};

void drawIcon(ImDrawList *list, Icon icon, ImVec2 center, float size, ImU32 tint) {
    const float thick = std::max(1.6f, size * 0.09f);
    switch (icon) {
    case Icon::Disc:
        list->AddCircle(center, size * 0.5f, tint, 40, thick);
        list->AddCircleFilled(center, size * 0.1f, tint);
        list->PathArcTo(center, size * 0.3f, -0.7f, 0.8f, 10);
        list->PathStroke(tint, 0, std::max(1.3f, size * 0.06f));
        break;
    case Icon::Wrench: {
        const ImVec2 jaw(center.x + size * 0.24f, center.y - size * 0.24f);
        list->PathArcTo(jaw, size * 0.2f, 0.4f, 4.6f, 10);
        list->PathStroke(tint, 0, std::max(2.0f, size * 0.13f));
        list->AddLine(ImVec2(jaw.x - size * 0.15f, jaw.y + size * 0.15f),
                      ImVec2(center.x - size * 0.32f, center.y + size * 0.32f), tint, std::max(2.0f, size * 0.15f));
        break;
    }
    case Icon::Sliders:
        for (int row = -1; row <= 1; ++row) {
            const float y = center.y + row * size * 0.27f;
            list->AddLine(ImVec2(center.x - size * 0.42f, y), ImVec2(center.x + size * 0.42f, y), tint,
                          std::max(1.5f, size * 0.07f));
            const float knob = row < 0 ? -0.22f : (row == 0 ? 0.1f : 0.28f);
            list->AddCircleFilled(ImVec2(center.x + size * knob, y), size * 0.1f, tint);
        }
        break;
    case Icon::Hammer:
        list->AddRectFilled(ImVec2(center.x - size * 0.36f, center.y - size * 0.4f),
                            ImVec2(center.x + size * 0.12f, center.y - size * 0.14f), tint, size * 0.08f);
        list->AddLine(ImVec2(center.x + size * 0.0f, center.y - size * 0.16f),
                      ImVec2(center.x + size * 0.32f, center.y + size * 0.4f), tint, std::max(2.2f, size * 0.13f));
        break;
    case Icon::Play:
        list->AddTriangleFilled(ImVec2(center.x - size * 0.26f, center.y - size * 0.34f),
                                ImVec2(center.x - size * 0.26f, center.y + size * 0.34f),
                                ImVec2(center.x + size * 0.36f, center.y), tint);
        break;
    case Icon::Check:
        list->AddLine(ImVec2(center.x - size * 0.28f, center.y + size * 0.02f),
                      ImVec2(center.x - size * 0.05f, center.y + size * 0.26f), tint, thick);
        list->AddLine(ImVec2(center.x - size * 0.05f, center.y + size * 0.26f),
                      ImVec2(center.x + size * 0.32f, center.y - size * 0.28f), tint, thick);
        break;
    case Icon::Close:
        list->AddLine(ImVec2(center.x - size * 0.25f, center.y - size * 0.25f),
                      ImVec2(center.x + size * 0.25f, center.y + size * 0.25f), tint, thick);
        list->AddLine(ImVec2(center.x + size * 0.25f, center.y - size * 0.25f),
                      ImVec2(center.x - size * 0.25f, center.y + size * 0.25f), tint, thick);
        break;
    case Icon::Warning:
        list->AddTriangleFilled(ImVec2(center.x, center.y - size * 0.36f),
                                ImVec2(center.x + size * 0.38f, center.y + size * 0.3f),
                                ImVec2(center.x - size * 0.38f, center.y + size * 0.3f), tint);
        list->AddLine(ImVec2(center.x, center.y - size * 0.16f), ImVec2(center.x, center.y + size * 0.08f),
                      IM_COL32(16, 20, 40, 255), std::max(2.0f, size * 0.1f));
        list->AddCircleFilled(ImVec2(center.x, center.y + size * 0.21f), std::max(1.8f, size * 0.06f),
                              IM_COL32(16, 20, 40, 255));
        break;
    case Icon::Folder:
        list->AddRectFilled(ImVec2(center.x - size * 0.4f, center.y - size * 0.3f),
                            ImVec2(center.x - size * 0.02f, center.y - size * 0.12f), tint, size * 0.04f);
        list->AddRectFilled(ImVec2(center.x - size * 0.4f, center.y - size * 0.16f),
                            ImVec2(center.x + size * 0.4f, center.y + size * 0.3f), tint, size * 0.06f);
        break;
    case Icon::Copy:
        list->AddRect(ImVec2(center.x - size * 0.32f, center.y - size * 0.32f),
                      ImVec2(center.x + size * 0.12f, center.y + size * 0.12f), tint, size * 0.05f, 0, thick);
        list->AddRectFilled(ImVec2(center.x - size * 0.08f, center.y - size * 0.08f),
                            ImVec2(center.x + size * 0.34f, center.y + size * 0.34f), IM_COL32(13, 17, 40, 216));
        list->AddRect(ImVec2(center.x - size * 0.08f, center.y - size * 0.08f),
                      ImVec2(center.x + size * 0.34f, center.y + size * 0.34f), tint, size * 0.05f, 0, thick);
        break;
    case Icon::Info:
        list->AddCircle(center, size * 0.36f, tint, 24, thick);
        list->AddLine(ImVec2(center.x, center.y - size * 0.14f), ImVec2(center.x, center.y + size * 0.1f), tint, thick);
        list->AddCircleFilled(ImVec2(center.x, center.y + size * 0.22f), std::max(1.8f, size * 0.055f), tint);
        break;
    case Icon::Star: {
        ImVec2 points[10];
        for (int i = 0; i < 10; ++i) {
            const float radius = (i % 2 == 0) ? size * 0.5f : size * 0.2f;
            const float angle = -M_PI / 2.0f + static_cast<float>(i) * (M_PI / 5.0f);
            points[i] = ImVec2(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
        }
        for (int i = 0; i < 10; ++i) {
            const ImVec2 &a = points[i], &b = points[(i + 1) % 10];
            list->AddTriangleFilled(center, a, b, tint);
        }
        break;
    }
    case Icon::Link:
        list->AddRect(ImVec2(center.x - size * 0.34f, center.y - size * 0.34f),
                      ImVec2(center.x + size * 0.34f, center.y + size * 0.34f), tint, size * 0.07f, 0, thick);
        list->AddLine(ImVec2(center.x - size * 0.12f, center.y + size * 0.12f),
                      ImVec2(center.x + size * 0.16f, center.y - size * 0.16f), tint, thick);
        list->AddLine(ImVec2(center.x + size * 0.16f, center.y - size * 0.16f),
                      ImVec2(center.x - size * 0.02f, center.y - size * 0.16f), tint, thick);
        list->AddLine(ImVec2(center.x + size * 0.16f, center.y - size * 0.16f),
                      ImVec2(center.x + size * 0.16f, center.y + size * 0.02f), tint, thick);
        break;
    case Icon::Reset:
        list->PathArcTo(center, size * 0.32f, 0.6f, 5.2f, 16);
        list->PathStroke(tint, 0, thick);
        list->AddTriangleFilled(
            ImVec2(center.x + size * 0.46f, center.y - size * 0.12f),
            ImVec2(center.x + size * 0.18f, center.y - size * 0.28f),
            ImVec2(center.x + size * 0.3f, center.y + size * 0.1f), tint);
        break;
    }
}

// ---------------------------------------------------------------- text

void title(const char *text, const char *intro) {
    pushFont(g_fonts.bold28);
    ImGui::TextUnformatted(text);
    popFont();
    if (intro) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kTextMuted));
        ImGui::TextUnformatted(intro);
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }
    ImGui::Dummy(ImVec2(0.0f, em(0.6f)));
}

void wrapped(ImU32 tint, const std::string &text) {
    if (text.empty())
        return;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(tint));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

void sectionHeader(const char *text) {
    ImGui::Dummy(ImVec2(0.0f, em(0.4f)));
    pushFont(g_fonts.bold);
    ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kGold));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    popFont();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y - em(0.28f)),
                                        ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y - em(0.28f)),
                                        palette::kBorderInner, 1.0f);
    ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
}

// A setting: its name and what it changes, with the control underneath.
void settingRow(const char *name, const char *help) {
    ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kText));
    ImGui::TextUnformatted(name);
    ImGui::PopStyleColor();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kTextMuted));
    ImGui::TextUnformatted(help);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0.0f, em(0.25f)));
}

// ---------------------------------------------------------------- shapes

void spinner(ImDrawList *list, ImVec2 center, float radius, double time, ImU32 tint) {
    const float start = static_cast<float>(std::fmod(time * 5.0, 6.2831853));
    list->PathArcTo(center, radius, start, start + 4.2f, 24);
    list->PathStroke(tint, std::max(1.5f, radius * 0.28f));
}

// A rounded bar, gold as it fills; a negative fraction sweeps while unknown.
void progressBar(ImDrawList *list, ImVec2 min, float width, double fraction, float height, double time) {
    const ImVec2 max(min.x + width, min.y + height);
    const float radius = height * 0.5f;
    list->AddRectFilled(min, max, palette::kFill, radius);
    if (fraction < 0.0) {
        const float span = width * 0.28f;
        const float x = min.x + static_cast<float>(std::fmod(time * 0.45, 1.0)) * (width + span) - span;
        list->PushClipRect(min, max, true);
        list->AddRectFilled(ImVec2(x, min.y), ImVec2(x + span, max.y), palette::withAlpha(palette::kGold, 190), radius);
        list->PopClipRect();
        return;
    }
    const float fill = std::max(height, width * static_cast<float>(std::clamp(fraction, 0.0, 1.0)));
    list->AddRectFilled(min, ImVec2(min.x + fill, max.y), palette::kGold, radius);
    const float shine = static_cast<float>(std::fmod(time * 0.35, 1.0)) * fill;
    list->PushClipRect(min, ImVec2(min.x + fill, max.y), true);
    list->AddRectFilledMultiColor(ImVec2(min.x + shine - height * 3.0f, min.y), ImVec2(min.x + shine, max.y),
                                  IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 84), IM_COL32(255, 255, 255, 84),
                                  IM_COL32(255, 255, 255, 0));
    list->PopClipRect();
}

void stageIcon(ImDrawList *list, ImVec2 center, float size, StageStatus status, double time) {
    switch (status) {
    case StageStatus::Done:
        list->AddCircleFilled(center, size * 0.5f, palette::withAlpha(palette::kGood, 56));
        drawIcon(list, Icon::Check, center, size * 0.8f, palette::kGood);
        break;
    case StageStatus::Skipped:
        list->AddCircleFilled(center, size * 0.5f, palette::kFill);
        drawIcon(list, Icon::Check, center, size * 0.8f, palette::kTextMuted);
        break;
    case StageStatus::Running:
        spinner(list, center, size * 0.42f, time, palette::kGold);
        break;
    case StageStatus::Failed:
        list->AddCircleFilled(center, size * 0.5f, palette::withAlpha(palette::kBad, 56));
        drawIcon(list, Icon::Close, center, size * 0.7f, palette::kBad);
        break;
    case StageStatus::Waiting:
        list->AddCircle(center, size * 0.36f, palette::withAlpha(palette::kTextMuted, 153), 24, 1.5f);
        break;
    }
}

void dashedRect(ImDrawList *list, ImVec2 min, ImVec2 max, ImU32 tint, float dash, float phase, float thickness) {
    const auto edge = [&](ImVec2 a, ImVec2 b) {
        const float length = std::hypot(b.x - a.x, b.y - a.y);
        if (length <= 0.0f)
            return;
        const ImVec2 step((b.x - a.x) / length, (b.y - a.y) / length);
        for (float at = std::fmod(phase, dash * 2.0f) - dash * 2.0f; at < length; at += dash * 2.0f) {
            const float from = std::max(0.0f, at), to = std::min(length, at + dash);
            if (to > from)
                list->AddLine(ImVec2(a.x + step.x * from, a.y + step.y * from),
                              ImVec2(a.x + step.x * to, a.y + step.y * to), tint, thickness);
        }
    };
    edge(min, ImVec2(max.x, min.y));
    edge(ImVec2(max.x, min.y), max);
    edge(max, ImVec2(min.x, max.y));
    edge(ImVec2(min.x, max.y), min);
}

// A read-only line of text in a dark well, with a button that copies it.
void commandWell(const std::string &command, double &copiedAt, double now) {
    const float pad = em(0.6f);
    const float buttonWidth = em(5.5f);
    const float width = ImGui::GetContentRegionAvail().x - buttonWidth - em(0.6f);
    const ImVec2 textSize = ImGui::CalcTextSize(command.c_str(), nullptr, false, width - pad * 2.0f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + textSize.y + pad * 2.0f);
    ImDrawList *list = ImGui::GetWindowDrawList();
    list->AddRectFilled(min, max, IM_COL32(4, 6, 18, 190), em(0.35f));
    list->AddRect(min, max, palette::kBorderInner, em(0.35f), 0, 1.0f);
    // The font overload carries wrap_width; nullptr is the default font.
    list->AddText(nullptr, 0.0f, ImVec2(min.x + pad, min.y + pad), palette::kText, command.c_str(), nullptr,
                  width - pad * 2.0f);
    ImGui::Dummy(ImVec2(width, max.y - min.y));
    ImGui::SameLine(0.0f, em(0.6f));
    const bool copied = now - copiedAt < 2.0;
    if (ImGui::Button(copied ? "Copied##cmd" : "Copy##cmd")) {
        SDL_SetClipboardText(command.c_str());
        copiedAt = now;
    }
}

// ---------------------------------------------------------------- buttons

// The part of a label that is drawn: everything before a ## id separator.
const char *labelEnd(const char *label) {
    const char *at = label;
    while (at[0] && !(at[0] == '#' && at[1] == '#'))
        ++at;
    return at;
}

float buttonWidth(const char *label) {
    return ImGui::GetStyle().FramePadding.x * 2.0f + em(1.0f) + em(0.45f) +
           ImGui::CalcTextSize(label, labelEnd(label)).x;
}

// The gold pill: the page's main action.
bool goldButton(const char *label, Icon icon, bool enabled = true) {
    const char *end = labelEnd(label);
    const ImVec2 pad(em(0.95f), em(0.55f));
    const ImVec2 textSize = ImGui::CalcTextSize(label, end);
    const float iconSize = em(1.0f);
    const ImVec2 size(textSize.x + iconSize + em(0.5f) + pad.x * 2.0f, textSize.y + pad.y * 2.0f);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hot = ImGui::IsItemHovered();
    ImDrawList *list = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const ImU32 light = hot ? IM_COL32(252, 218, 126, 255) : palette::kGold;
    const ImU32 deep = hot ? IM_COL32(228, 176, 60, 255) : palette::kGoldDeep;
    list->AddRectFilledMultiColor(min, max, light, light, deep, deep);
    list->AddRect(min, max, IM_COL32(255, 246, 216, 220), size.y * 0.5f, 0, 1.4f);
    drawIcon(list, icon, ImVec2(min.x + pad.x + iconSize * 0.5f, min.y + size.y * 0.5f), iconSize,
             IM_COL32(42, 28, 2, 255));
    list->AddText(ImVec2(min.x + pad.x + iconSize + em(0.5f), min.y + (size.y - textSize.y) * 0.5f),
                  IM_COL32(42, 28, 2, 255), label, end);
    ImGui::EndDisabled();
    return pressed && enabled;
}

// A quiet button for everything else.
bool darkButton(const char *label, Icon icon, bool enabled = true) {
    const char *end = labelEnd(label);
    const ImVec2 pad(em(0.85f), em(0.5f));
    const ImVec2 textSize = ImGui::CalcTextSize(label, end);
    const float iconSize = em(0.95f);
    const ImVec2 size(textSize.x + iconSize + em(0.45f) + pad.x * 2.0f, textSize.y + pad.y * 2.0f);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hot = ImGui::IsItemHovered();
    ImDrawList *list = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    list->AddRectFilled(min, max, hot ? palette::kFillStrong : palette::kFill, size.y * 0.5f);
    list->AddRect(min, max, hot ? palette::kGold : palette::kBorder, size.y * 0.5f, 0, 1.2f);
    drawIcon(list, icon, ImVec2(min.x + pad.x + iconSize * 0.5f, min.y + size.y * 0.5f), iconSize, palette::kGold);
    list->AddText(ImVec2(min.x + pad.x + iconSize + em(0.45f), min.y + (size.y - textSize.y) * 0.5f), palette::kText,
                  label, end);
    ImGui::EndDisabled();
    return pressed && enabled;
}

// A page in the sidebar: icon, name, a gold bar when it is the current page.
bool navItem(const char *label, Icon icon, bool active) {
    const char *end = labelEnd(label);
    const float height = em(2.3f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hot = ImGui::IsItemHovered();
    ImDrawList *list = ImGui::GetWindowDrawList();
    if (active || hot)
        list->AddRectFilled(at, ImVec2(at.x + width, at.y + height), active ? palette::kFillStrong : palette::kFill,
                            em(0.5f));
    if (active)
        list->AddRectFilled(ImVec2(at.x, at.y + height * 0.24f), ImVec2(at.x + 3.5f, at.y + height * 0.76f),
                            palette::kGold, 1.75f);
    drawIcon(list, icon, ImVec2(at.x + em(1.1f), at.y + height * 0.5f), em(1.2f),
             active ? palette::kGold : palette::kTextMuted);
    const ImVec2 textSize = ImGui::CalcTextSize(label, end);
    list->AddText(ImVec2(at.x + em(2.3f), at.y + (height - textSize.y) * 0.5f),
                  active ? palette::kText : palette::kTextMuted, label, end);
    return clicked;
}

} // namespace

// ---------------------------------------------------------------- shared

Fonts g_fonts{};

void pushFont(const ImFont *font) {
    ImFont *push = font ? const_cast<ImFont *>(font) : ImGui::GetIO().Fonts->Fonts[0];
    ImGui::PushFont(push);
}

void popFont() { ImGui::PopFont(); }

const char *const kPageNames[static_cast<size_t>(Page::Count)] = {"Your disc", "Tools", "Options", "Build", "Play"};

std::string humanSize(uint64_t bytes) {
    char text[32];
    if (bytes >= 1000000000ull)
        std::snprintf(text, sizeof text, "%.2f GB", static_cast<double>(bytes) / 1e9);
    else if (bytes >= 1000000ull)
        std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / 1e6);
    else
        std::snprintf(text, sizeof text, "%.1f KB", static_cast<double>(bytes) / 1e3);
    return text;
}

std::string duration(double seconds) {
    char text[48];
    if (seconds < 0.0)
        return {};
    if (seconds < 60.0)
        std::snprintf(text, sizeof text, "%.0f s", seconds);
    else if (seconds < 3600.0)
        std::snprintf(text, sizeof text, "%.0f min", std::ceil(seconds / 60.0));
    else
        std::snprintf(text, sizeof text, "%d h %02d min", static_cast<int>(seconds / 3600.0),
                      static_cast<int>(std::fmod(seconds, 3600.0) / 60.0));
    return text;
}

std::string shellQuote(const std::string &text) {
    std::string quoted = "'";
    for (const char c : text) {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted += c;
    }
    quoted += "'";
    return quoted;
}

std::filesystem::path configPath() {
    char *folder = SDL_GetPrefPath("TagForceRecomp", "Launcher");
    if (!folder)
        return "tagforce-launcher.ini";
    std::filesystem::path path = std::filesystem::path(folder) / "launcher.ini";
    SDL_free(folder);
    return path;
}

LauncherConfig loadConfig() {
    LauncherConfig config;
    std::ifstream file(configPath());
    std::string line;
    while (std::getline(file, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = line.substr(0, equals), value = line.substr(equals + 1u);
        if (key == "disc")
            config.disc = value;
        else if (key == "workspace")
            config.workspace = value;
        else if (key == "jobs")
            config.jobs = std::atoi(value.c_str());
    }
    return config;
}

void saveConfig(const LauncherConfig &config) {
    std::ofstream file(configPath(), std::ios::trunc);
    for (const char c : config.disc)
        if (c != '\n' && c != '\r')
            file << c;
    file << "\nworkspace=";
    for (const char c : config.workspace)
        if (c != '\n' && c != '\r')
            file << c;
    file << "\njobs=" << config.jobs << '\n';
}

int defaultJobs() {
    const int cores = std::max(1, SDL_GetCPUCount());
    const int memory = std::max(1, SDL_GetSystemRAM() / 1024); // 1 GB a job
    return std::clamp(std::min(cores, memory), 1, cores);
}

std::string defaultGamesFolder() {
    const char *home = SDL_getenv("HOME");
    return home ? (std::string(home) + "/TagForceRecomp") : std::string("TagForceRecomp");
}

std::optional<Payload> findPayload() {
    const char *base = SDL_GetBasePath();
    if (!base)
        return std::nullopt;
    const std::filesystem::path dir = std::filesystem::path(base) / "payload";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(dir / "payload.json", ec))
        return std::nullopt;
    std::string version;
    if (FILE *file = fopen((dir / "payload.json").string().c_str(), "rb")) {
        std::string text;
        char buffer[4096];
        size_t got;
        while ((got = fread(buffer, 1, sizeof buffer - 1, file)) > 0)
            text.append(buffer, got);
        fclose(file);
        const size_t key = text.find("\"version\"");
        if (key != std::string::npos) {
            const size_t colon = text.find(':', key);
            const size_t open = text.find('"', colon);
            const size_t close = open == std::string::npos ? std::string::npos : text.find('"', open + 1);
            if (close != std::string::npos)
                version = text.substr(open + 1, close - open - 1);
        }
    }
    Payload payload;
    payload.version = version.empty() ? "release" : version;
    payload.source = dir / "source";
    return payload;
}

const char *previewName(Preview preview) {
    switch (preview) {
    case Preview::DiscEmpty: return "disc-empty";
    case Preview::DiscReady: return "disc-ready";
    case Preview::ToolsReady: return "tools-ready";
    case Preview::ToolsMissing: return "tools-missing";
    case Preview::Options: return "options";
    case Preview::Building: return "building";
    case Preview::BuildFailed: return "build-failed";
    case Preview::Play: return "play";
    case Preview::Count: break;
    }
    return "";
}

// ---------------------------------------------------------------- app

LauncherApp::LauncherApp(std::filesystem::path repo, std::optional<Payload> payload, SDL_Window *window,
                         const LauncherConfig &overrides, bool persist)
    : m_repo(std::move(repo)), m_payload(payload), m_window(window), m_config(loadConfig()), m_persist(persist) {
    if (!overrides.disc.empty())
        m_config.disc = overrides.disc;
    if (!overrides.workspace.empty())
        m_config.workspace = overrides.workspace;
    if (overrides.jobs > 0)
        m_config.jobs = overrides.jobs;
    if (m_config.workspace.empty())
        m_config.workspace = m_payload ? defaultGamesFolder() : m_repo.parent_path().string();
    if (m_config.jobs <= 0)
        m_config.jobs = defaultJobs();
    std::snprintf(m_typedPath, sizeof m_typedPath, "%s", m_config.disc.c_str());
    std::snprintf(m_typedWorkspace, sizeof m_typedWorkspace, "%s", m_config.workspace.c_str());
    m_hasPicker = (std::system("command -v zenity >/dev/null 2>&1") == 0);
    std::error_code ec;
    if (!m_config.disc.empty() && std::filesystem::is_regular_file(m_config.disc, ec))
        setDisc(m_config.disc);
    checkToolsAsync();
    if (m_disc && m_disc->supported)
        m_page = pageDone(Page::Build) ? Page::Play : Page::Tools;
}

LauncherApp::~LauncherApp() {
    m_pipeline.cancel();
    if (m_toolsThread.joinable())
        m_toolsThread.join();
}

std::filesystem::path LauncherApp::installDir() const {
    const char *base = SDL_GetBasePath();
    if (base)
        return std::filesystem::path(base).parent_path();
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

std::filesystem::path LauncherApp::sourceRoot() const { return m_payload ? m_payload->source : m_repo; }

std::filesystem::path LauncherApp::gamePath() const { return installDir() / "TagForceRecomp"; }

void LauncherApp::handleEvent(const SDL_Event &event) {
    switch (event.type) {
    case SDL_DROPBEGIN:
        m_dragging = true;
        break;
    case SDL_DROPTEXT:
    case SDL_DROPFILE: {
        m_dragging = false;
        char *file = event.drop.file;
        if (file && !m_pipeline.running()) {
            setDisc(file);
            m_page = Page::Disc;
        }
        if (file)
            SDL_free(file);
        break;
    }
    case SDL_DROPCOMPLETE:
        m_dragging = false;
        break;
    default:
        break;
    }
}

void LauncherApp::setDisc(const std::string &path) {
    m_disc = inspectDisc(path);
    m_config.disc = path;
    std::snprintf(m_typedPath, sizeof m_typedPath, "%s", path.c_str());
    if (m_live && m_persist)
        saveConfig(m_config);
}

void LauncherApp::checkToolsAsync() {
    if (m_checkingTools.exchange(true))
        return;
    if (m_toolsThread.joinable())
        m_toolsThread.join();
    m_toolsThread = std::thread([this] {
        ToolReport report = checkTools();
        std::lock_guard lock(m_toolsMutex);
        m_tools = std::move(report);
        m_checkingTools = false;
    });
}

// zenity, when it is there, picks a file or a folder on a thread of its own.
void LauncherApp::browseDisc() {
    {
        std::lock_guard lock(m_picks->mutex);
        if (m_picks->browsing)
            return;
        m_picks->browsing = true;
    }
    std::thread([picks = m_picks] {
        FILE *pipe =
            popen("zenity --file-selection --title 'Choose your disc image' --file-filter 'Disc images | *.iso *.ISO' 2>/dev/null", "r");
        std::string path;
        if (pipe) {
            char line[4096];
            while (fgets(line, sizeof line, pipe)) {
                const size_t length = strlen(line);
                while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
                    line[length - 1] = '\0';
                if (line[0])
                    path = line;
            }
            pclose(pipe);
        }
        std::lock_guard lock(picks->mutex);
        if (!path.empty())
            picks->disc = path;
        picks->browsing = false;
    }).detach();
}

void LauncherApp::browseWorkspace() {
    {
        std::lock_guard lock(m_picks->mutex);
        if (m_picks->browsing)
            return;
        m_picks->browsing = true;
    }
    std::thread([picks = m_picks] {
        FILE *pipe = popen("zenity --file-selection --directory --title 'Choose the game files folder' 2>/dev/null", "r");
        std::string path;
        if (pipe) {
            char line[4096];
            while (fgets(line, sizeof line, pipe)) {
                const size_t length = strlen(line);
                while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
                    line[length - 1] = '\0';
                if (line[0])
                    path = line;
            }
            pclose(pipe);
        }
        std::lock_guard lock(picks->mutex);
        if (!path.empty())
            picks->workspace = path;
        picks->browsing = false;
    }).detach();
}

bool LauncherApp::pageDone(Page page) const {
    switch (page) {
    case Page::Disc:
        return m_disc && m_disc->supported;
    case Page::Tools: {
        std::lock_guard lock(m_toolsMutex);
        return m_tools && m_tools->ready();
    }
    case Page::Options:
        return true;
    case Page::Build: {
        if (!m_live)
            return m_page == Page::Play;
        if (m_pipeline.running())
            return false;
        if (m_time - m_builtCheckedAt > 0.5 || m_time < m_builtCheckedAt) {
            std::error_code ec;
            m_built = std::filesystem::is_regular_file(gamePath(), ec);
            m_builtCheckedAt = m_time;
        }
        return m_built || m_pipeline.succeeded() || m_pipeline.stoppedAtGap();
    }
    case Page::Play:
    case Page::Count:
        break;
    }
    return false;
}

bool LauncherApp::pageReachable(Page page) const {
    switch (page) {
    case Page::Disc:
        return true;
    case Page::Tools:
        return pageDone(Page::Disc);
    case Page::Options:
    case Page::Build:
        return pageDone(Page::Disc) && pageDone(Page::Tools);
    case Page::Play:
        return pageDone(Page::Build);
    case Page::Count:
        break;
    }
    return false;
}

void LauncherApp::startBuild() {
    if (!m_live || !m_persist || m_pipeline.running() || !m_disc)
        return;
    saveConfig(m_config);
    PipelineOptions options;
    options.source = sourceRoot();
    options.workspace = m_config.workspace;
    options.disc = m_config.disc;
    options.jobs = m_config.jobs;
    m_showLog = m_revealLog = true;
    m_pipeline.start(options);
}

void LauncherApp::launchGame() {
    m_launchError.clear();
    if (!m_live)
        return;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(gamePath(), ec)) {
        m_launchError =
            "The Linux runtime is not part of this preview: Build has prepared your game files and translated code, "
            "and this starts the game as soon as a Linux build of TagForceRecomp sits beside the launcher (in " +
            installDir().string() + ").";
        return;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        const std::string log = (std::filesystem::path(m_config.workspace) / "tagforce.log").string();
        freopen(log.c_str(), "w", stdout);
        freopen(log.c_str(), "a", stderr);
        const std::string game = gamePath().string();
        const std::string data = (std::filesystem::path(m_config.workspace) / "jogo").string();
        execl(game.c_str(), game.c_str(), "--dados", data.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    if (pid < 0)
        m_launchError = "The game did not start.";
}

std::string LauncherApp::bugReport() const {
#if defined(__aarch64__)
    constexpr const char *kArch = "arm64";
#else
    constexpr const char *kArch = "x86-64";
#endif
    std::ostringstream report;
    report << "Tag Force Recomp Linux launcher, "
           << (m_payload ? "release " + m_payload->version : std::string("from a checkout")) << "\nSystem: Linux "
           << kArch << ", " << SDL_GetCPUCount() << " logical cores, " << SDL_GetSystemRAM() << " MB of memory, "
           << m_config.jobs << " compile jobs\n";
    {
        std::lock_guard lock(m_toolsMutex);
        if (m_tools) {
            report << "Tools:";
            for (const ToolCheck &tool : m_tools->tools)
                report << ' ' << tool.name << ' ' << (tool.found ? tool.version : std::string("missing")) << ';';
            report << '\n';
        }
    }
    const std::string error = m_pipeline.error();
    if (!error.empty())
        report << "Error: " << error << '\n';
    const auto stages = m_pipeline.stages();
    for (size_t i = 0; i < kStageCount; ++i) {
        report << "- " << stageTitle(static_cast<Stage>(i)) << ": " << statusName(stages[i].status);
        if (!stages[i].detail.empty())
            report << ", " << stages[i].detail;
        report << '\n';
    }
    report << "\nThe end of the launcher's log:\n```\n";
    for (const std::string &line : m_pipeline.log(200))
        report << line << '\n';
    report << "```\n";
    return report.str();
}

void LauncherApp::showPreview(Preview preview) {
    m_live = false;
    if (m_toolsThread.joinable())
        m_toolsThread.join();
    m_checkingTools = false;
    m_hasPicker = true; // the screenshots show the picker, not the typed-path fallback
    // Never the player's own paths or settings in a screenshot.
    m_config.disc = "/home/you/Games/Yu-Gi-Oh! GX Tag Force (Europe).iso";
    m_config.workspace = "/home/you/TagForceRecomp";
    m_config.jobs = 8;
    DiscInfo disc;
    disc.readable = true;
    disc.volume = "TAGFORCE";
    disc.gameId = kGameId;
    disc.size = static_cast<uint64_t>(kDiscPreviewSize);
    disc.supported = true;
    m_disc = disc;

    ToolReport tools;
    const auto add = [&](const char *name, const char *purpose, const char *version, bool optional = false) {
        ToolCheck check;
        check.name = name;
        check.purpose = purpose;
        check.version = version ? version : "";
        check.found = version != nullptr;
        check.optional = optional;
        if (!check.found)
            check.problem = "Not found. " + std::string(optional ? "The game works without it." : "Install it and press Check again.");
        tools.tools.push_back(check);
    };
    const bool missing = preview == Preview::ToolsMissing;
    add("C++ compiler", "Compiles the game", missing ? nullptr : "13.2.0");
    add("CMake", "Configures the runtime build", missing ? nullptr : "3.28.3");
    add("Ninja", "Runs the build", missing ? nullptr : "1.11.1");
    add("Python 3", "Copies the disc and translates the game code", "3.12.3");
    add("FFmpeg", "Plays the movies and music; without it they stay silent", missing ? nullptr : "6.1.1", true);
    if (missing) {
        tools.installCommand = "sudo apt install build-essential cmake ninja-build python3 ffmpeg";
        tools.instructions = "Your distribution's packages have them all. Copy the line below into a terminal, enter "
                             "your password, and press Check again when it finishes.";
    }
    {
        std::lock_guard lock(m_toolsMutex);
        m_tools = tools;
    }

    m_previewStages = {};
    m_previewError.clear();
    m_previewLog.clear();
    m_logText.clear();
    m_logTextVersion = UINT64_MAX;
    m_launchError.clear();
    const auto done = [&](Stage stage, double seconds, const char *detail) {
        StageState &state = m_previewStages[static_cast<size_t>(stage)];
        state.status = StageStatus::Done;
        state.progress = 1.0;
        state.seconds = seconds;
        state.detail = detail;
    };
    switch (preview) {
    case Preview::DiscEmpty:
        m_disc.reset();
        m_page = Page::Disc;
        break;
    case Preview::DiscReady:
        m_page = Page::Disc;
        break;
    case Preview::ToolsReady:
    case Preview::ToolsMissing:
        m_page = Page::Tools;
        break;
    case Preview::Options:
        m_page = Page::Options;
        break;
    case Preview::Building:
    case Preview::BuildFailed: {
        m_page = Page::Build;
        m_showLog = true;
        done(Stage::CheckDisc, 1.8, "ULES-00600  1.07 GB");
        done(Stage::UnpackSource, 2.6, "into /home/you/TagForceRecomp/source");
        done(Stage::CopyFiles, 34.0, "1.07 GB into /home/you/TagForceRecomp/jogo");
        done(Stage::TranslateCode, 48.0, "40 of 40 modules");
        StageState &compile = m_previewStages[static_cast<size_t>(Stage::CompileRuntime)];
        compile.status = preview == Preview::Building ? StageStatus::Running : StageStatus::Failed;
        compile.progress = 0.38;
        compile.seconds = 21.0;
        compile.startedAt = -1.0;
        compile.detail = "cmake --build build-linux";
        m_previewLog = {"$ cd /home/you/TagForceRecomp/source && cmake -S . -B build-linux -G Ninja",
                        "-- The C compiler identification is GNU 13.2.0",
                        "-- Configuring done (1.2s)",
                        "$ cmake --build build-linux -j 8",
                        "[12/57] Building CXX object runtime/core/kernel.cpp.o"};
        if (preview == Preview::BuildFailed) {
            // The essentials fit the box: the tail, where the failure speaks.
            m_previewLog = {"$ cd /home/you/TagForceRecomp/source && cmake -S . -B build-linux -G Ninja",
                            "$ cmake --build build-linux -j 8",
                            "g++: internal compiler error: Killed (program cc1plus)",
                            "ninja: build stopped: subcommand failed."};
            m_previewError = "Compiling the runtime failed; the log shows why.";
        }
        break;
    }
    case Preview::Play:
        m_page = Page::Play;
        for (size_t i = 0; i < kStageCount; ++i)
            m_previewStages[i].status = StageStatus::Done;
        break;
    case Preview::Count:
        break;
    }
}

// ---------------------------------------------------------------- drawing

void LauncherApp::drawBackground() {
    ImDrawList *list = ImGui::GetBackgroundDrawList();
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    list->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(9, 13, 38, 255), IM_COL32(16, 24, 64, 255),
                                  IM_COL32(28, 20, 50, 255), IM_COL32(7, 9, 26, 255));
    // A few slow stars: the game's own menus sit over a night sky.
    for (int i = 0; i < 70; ++i) {
        const float x = std::fmod(static_cast<float>(i) * 197.31f, 1.0f * size.x);
        const float y = std::fmod(static_cast<float>(i * i) * 61.7f + static_cast<float>(i) * 13.0f, size.y * 0.75f);
        const float twinkle = 0.45f + 0.55f * std::sin(static_cast<float>(m_time) * (0.6f + (i % 7) * 0.13f) + i);
        list->AddCircleFilled(ImVec2(x, y), 0.6f + (i % 3) * 0.5f,
                              IM_COL32(255, 255, 255, static_cast<int>(40 + 90 * twinkle * twinkle)));
    }
    list->AddRectFilledMultiColor(ImVec2(0, size.y * 0.72f), size, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                  palette::withAlpha(palette::kGold, 24), palette::withAlpha(palette::kGold, 24));
}

void LauncherApp::draw() {
    m_time = ImGui::GetTime();

    // The picker threads answer in their own time.
    std::optional<std::string> pickedDisc, pickedWorkspace;
    {
        std::lock_guard lock(m_picks->mutex);
        pickedDisc = std::exchange(m_picks->disc, std::nullopt);
        pickedWorkspace = std::exchange(m_picks->workspace, std::nullopt);
    }
    if (pickedDisc) {
        setDisc(*pickedDisc);
        m_page = Page::Disc;
    }
    if (pickedWorkspace) {
        m_config.workspace = *pickedWorkspace;
        std::snprintf(m_typedWorkspace, sizeof m_typedWorkspace, "%s", m_config.workspace.c_str());
        if (m_live && m_persist)
            saveConfig(m_config);
    }
    // A finished build moves on to Play by itself; a failed one shows its log.
    if (m_live && m_page == Page::Build && m_pipeline.succeeded() && !m_pipeline.running())
        m_page = Page::Play;
    if (m_live) {
        const bool running = m_pipeline.running();
        if (m_buildWasRunning && !running && !m_pipeline.succeeded())
            m_showLog = m_revealLog = true;
        m_buildWasRunning = running;
    }

    drawBackground();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float margin = em(1.4f);
    const ImVec2 min(margin, margin), max(display.x - margin, display.y - margin);
    ImDrawList *back = ImGui::GetBackgroundDrawList();
    for (int i = 3; i >= 1; --i) { // a soft shadow behind the card
        const float grow = em(0.28f) * i;
        back->AddRectFilled(ImVec2(min.x - grow, min.y - grow + em(0.2f) * i), ImVec2(max.x + grow, max.y + grow),
                            IM_COL32(0, 0, 0, 14 + 10 * (4 - i)), em(0.9f));
    }
    back->AddRectFilled(min, max, palette::kWindow, em(0.8f));
    back->AddRect(min, max, palette::kBorder, em(0.8f), 0, 1.2f);

    ImGui::SetNextWindowPos(ImVec2(min.x + em(1.2f), min.y + em(1.0f)));
    ImGui::SetNextWindowSize(ImVec2(max.x - min.x - em(2.4f), max.y - min.y - em(2.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f)); // content fills the card exactly
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    // Header: the star, the name, and what this is for.
    drawIcon(ImGui::GetWindowDrawList(), Icon::Star, ImGui::GetCursorScreenPos() + ImVec2(em(1.1f), em(1.1f)), em(2.2f),
             palette::kGold);
    ImGui::Dummy(ImVec2(em(2.4f), 0.0f));
    ImGui::SameLine(0.0f, em(0.55f));
    ImGui::BeginGroup();
    pushFont(g_fonts.bold28);
    ImGui::TextUnformatted("TagForceRecomp");
    popFont();
    ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kTextMuted));
    ImGui::TextUnformatted("Yu-Gi-Oh! GX Tag Force, native on Linux, from your own disc");
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0.0f, em(0.25f)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, em(0.45f)));

    const float sidebarWidth = em(13.5f);
    const float bodyHeight = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##sidebar", ImVec2(sidebarWidth, bodyHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    drawSidebar();
    ImGui::EndChild();
    ImGui::SameLine(0.0f, em(1.2f));
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x - em(0.6f), at.y), ImVec2(at.x - em(0.6f), at.y + bodyHeight),
                                            palette::kBorderInner, 1.0f);
    }
    ImGui::BeginChild("##page", ImVec2(0.0f, bodyHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    // Real slack for the footer, and a child id per page: a page that scrolls
    // (Build, with its log) must not leave its scroll on the next page.
    char bodyId[32];
    std::snprintf(bodyId, sizeof bodyId, "##page-body-%d", static_cast<int>(m_page));
    ImGui::BeginChild(bodyId, ImVec2(0.0f, ImGui::GetContentRegionAvail().y - em(3.6f)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    switch (m_page) {
    case Page::Disc: drawDiscPage(); break;
    case Page::Tools: drawToolsPage(); break;
    case Page::Options: drawOptionsPage(); break;
    case Page::Build: drawBuildPage(); break;
    case Page::Play: drawPlayPage(); break;
    case Page::Count: break;
    }
    ImGui::EndChild();
    drawFooter();
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleVar();
}

void LauncherApp::drawSidebar() {
    static constexpr Icon kPageIcons[] = {Icon::Disc, Icon::Wrench, Icon::Sliders, Icon::Hammer, Icon::Play};
    for (int index = 0; index < static_cast<int>(Page::Count); ++index) {
        const auto page = static_cast<Page>(index);
        const bool reachable = pageReachable(page);
        ImGui::BeginDisabled(!reachable);
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        if (navItem(kPageNames[index], kPageIcons[index], m_page == page))
            goTo(page);
        ImGui::EndDisabled();
        if (reachable && pageDone(page) && page != Page::Play && page != Page::Options) {
            const float height = em(2.3f);
            drawIcon(ImGui::GetWindowDrawList(), Icon::Check,
                     ImVec2(top.x + rowWidth - em(1.1f), top.y + height * 0.5f), em(1.0f), palette::kGood);
        }
    }
    ImGui::Dummy(ImVec2(0.0f, em(1.0f)));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kTextMuted));
    ImGui::TextUnformatted("Your disc and your saves stay on this computer. Nothing from the game is downloaded "
                           "or shared.");
    ImGui::Dummy(ImVec2(0.0f, em(0.6f)));
    ImGui::Text("Launcher: %s", m_payload ? ("release " + m_payload->version).c_str() : "local checkout");
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

void LauncherApp::drawFooter() {
    ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
    const ImVec2 row = ImGui::GetCursorPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const int index = static_cast<int>(m_page);
    if (index > 0 && !(m_live && m_pipeline.running())) {
        if (darkButton("Back##footer", Icon::Reset))
            m_page = static_cast<Page>(index - 1);
    }
    // The page's own actions, right-aligned and always in view.
    struct Action {
        const char *label;
        Icon icon;
        bool primary, enabled;
        int id; // 0 details, 1 cancel, 2 build, 3 continue
    };
    std::vector<Action> actions;
    if (m_page == Page::Build) {
        const auto stages = m_live ? m_pipeline.stages() : m_previewStages;
        const bool running = std::any_of(stages.begin(), stages.end(), [](const StageState &stage) {
            return stage.status == StageStatus::Running;
        });
        const bool started = m_live ? m_pipeline.started()
                                    : std::any_of(stages.begin(), stages.end(), [](const StageState &stage) {
                                          return stage.status != StageStatus::Waiting;
                                      });
        actions.push_back({m_showLog ? "Hide details" : "Show details", Icon::Info, false, true, 0});
        if (running)
            actions.push_back({"Cancel##footer", Icon::Close, false, m_live, 1});
        else
            actions.push_back({started ? "Build again" : "Start building", Icon::Hammer, true, m_live, 2});
    } else if (m_page != Page::Play) {
        const auto next = static_cast<Page>(index + 1);
        actions.push_back({"Continue##footer", Icon::Play, true, pageDone(m_page) && pageReachable(next), 3});
    }
    float total = 0.0f;
    for (const Action &action : actions)
        total += buttonWidth(action.label);
    if (actions.size() > 1)
        total += em(0.6f) * static_cast<float>(actions.size() - 1);
    if (!actions.empty()) {
        ImGui::SetCursorPos(ImVec2(row.x + width - total - em(0.6f), row.y)); // clear of the card's edge
        for (const Action &action : actions) {
            const bool pressed = action.primary ? goldButton(action.label, action.icon, action.enabled)
                                                : darkButton(action.label, action.icon, action.enabled);
            ImGui::SameLine(0.0f, em(0.6f));
            if (!pressed)
                continue;
            switch (action.id) {
            case 0: m_showLog = m_revealLog = !m_showLog; break;
            case 1:
                if (m_live)
                    m_pipeline.cancel();
                break;
            case 2: startBuild(); break;
            case 3: goTo(static_cast<Page>(index + 1)); break;
            default: break;
            }
        }
        // Ends the row the buttons and their SameLine()s left open; without
        // them (the Play page) a NewLine would advance one line too many.
        ImGui::NewLine();
    }
}

// ---------------------------------------------------------------- pages

void LauncherApp::drawDiscPage() {
    title("Your disc",
          "Tag Force Recomp turns your own copy of Yu-Gi-Oh! GX Tag Force into a game that runs natively on this "
          "computer. It needs a disc image (a .iso file) of the European release, ULES-00600. Nothing from the game "
          "comes with the download.");
    ImDrawList *list = ImGui::GetWindowDrawList();
    const bool ready = m_disc && m_disc->supported;
    if (!ready) {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = em(12.5f);
        const ImVec2 max(min.x + width, min.y + height);
        const bool hot = m_dragging || ImGui::IsMouseHoveringRect(min, max);
        list->AddRectFilled(min, max, hot ? palette::withAlpha(palette::kGold, 26) : palette::kFill, em(0.8f));
        dashedRect(list, ImVec2(min.x + 2, min.y + 2), ImVec2(max.x - 2, max.y - 2),
                   hot ? palette::kGold : palette::withAlpha(palette::kTextMuted, 179), em(0.6f),
                   static_cast<float>(m_time) * (hot ? 24.0f : 8.0f), 2.0f);
        const ImVec2 center(min.x + width * 0.5f, min.y + em(3.8f));
        drawIcon(list, Icon::Disc, center, em(4.0f), hot ? palette::kGold : palette::kText);
        const char *line = m_dragging ? "Drop it here" : "Drop your disc image here";
        const char *alternative = "or choose it from a folder";
        pushFont(g_fonts.bold22);
        const ImVec2 lineSize = ImGui::CalcTextSize(line);
        list->AddText(g_fonts.bold22, g_fonts.bold22 ? g_fonts.bold22->FontSize : 0.0f,
                      ImVec2(center.x - lineSize.x * 0.5f, min.y + em(6.3f)), palette::kText, line);
        popFont();
        const ImVec2 alternativeSize = ImGui::CalcTextSize(alternative);
        list->AddText(ImVec2(center.x - alternativeSize.x * 0.5f, min.y + em(8.0f)), palette::kTextMuted, alternative);
        bool browsing = false;
        {
            std::lock_guard lock(m_picks->mutex);
            browsing = m_picks->browsing;
        }
        if (browsing) {
            spinner(list, ImVec2(center.x - em(3.6f), min.y + em(9.6f) + em(0.55f)), em(0.55f), m_time, palette::kGold);
            ImGui::SetCursorScreenPos(ImVec2(center.x - em(2.0f), min.y + em(9.6f) + em(0.28f)));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Choosing...");
        } else {
            ImGui::SetCursorScreenPos(ImVec2(center.x - buttonWidth("Browse...") * 0.5f, min.y + em(9.6f)));
            if (goldButton("Browse...", Icon::Folder))
                browseDisc();
        }
        ImGui::SetCursorScreenPos(ImVec2(min.x, max.y + em(0.8f)));
        ImGui::Dummy(ImVec2(0.0f, 0.0f)); // the zone counts toward the page's size
        if (!m_hasPicker && !m_dragging) {
            // No system picker: the path can be typed instead.
            ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kTextMuted));
            ImGui::TextUnformatted("or type the path to it:");
            ImGui::PopStyleColor();
            ImGui::PushItemWidth(em(26.0f));
            if (ImGui::InputText("##disc-path", m_typedPath, sizeof m_typedPath, ImGuiInputTextFlags_EnterReturnsTrue))
                if (m_typedPath[0])
                    setDisc(m_typedPath);
            ImGui::PopItemWidth();
            ImGui::SameLine(0.0f, em(0.5f));
            if (darkButton("Check##disc", Icon::Check))
                if (m_typedPath[0])
                    setDisc(m_typedPath);
        }
        if (m_disc && !m_disc->error.empty()) {
            ImGui::Dummy(ImVec2(0.0f, em(0.4f)));
            drawIcon(list, Icon::Warning, ImGui::GetCursorScreenPos() + ImVec2(em(0.6f), em(0.6f)), em(1.2f),
                     palette::kBad);
            ImGui::Dummy(ImVec2(em(1.5f), 0.0f));
            ImGui::SameLine(0.0f, em(0.3f));
            wrapped(palette::kBad, m_disc->error);
        }
        return;
    }

    // The recognised disc, as a card.
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = em(9.6f);
    const ImVec2 max(min.x + width, min.y + height);
    list->AddRectFilled(min, max, palette::kFillStrong, em(0.8f));
    list->AddRect(min, max, palette::withAlpha(palette::kGold, 217), em(0.8f), 0, 1.5f);
    drawIcon(list, Icon::Disc, ImVec2(min.x + em(3.2f), min.y + height * 0.5f), em(4.2f), palette::kGold);
    ImGui::SetCursorScreenPos(ImVec2(min.x + em(6.4f), min.y + em(1.0f)));
    ImGui::BeginGroup();
    pushFont(g_fonts.bold22);
    ImGui::TextUnformatted(kGameTitle);
    popFont();
    ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kTextMuted));
    ImGui::Text("European release  %s  %s", m_disc->gameId.c_str(), humanSize(m_disc->size).c_str());
    ImGui::PushTextWrapPos(max.x - em(1.0f));
    ImGui::TextUnformatted(m_config.disc.c_str());
    ImGui::PopTextWrapPos();
    if (!m_disc->volume.empty())
        ImGui::Text("volume %s", m_disc->volume.c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
    drawIcon(list, Icon::Check, ImGui::GetCursorScreenPos() + ImVec2(em(0.55f), em(0.55f)), em(1.1f), palette::kGood);
    ImGui::Dummy(ImVec2(em(1.3f), 0.0f));
    ImGui::SameLine(0.0f, em(0.3f));
    ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kGood));
    ImGui::TextUnformatted("Recognised. Every file is copied from it when the build starts.");
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(min.x, max.y + em(1.0f)));
    if (darkButton("Choose another disc...", Icon::Folder))
        browseDisc();
}

void LauncherApp::drawToolsPage() {
    title("Tools", "Building the game takes a few free developer tools. The launcher looks for them on this "
                   "computer; what is missing, your distribution installs in one line.");
    std::optional<ToolReport> report;
    {
        std::lock_guard lock(m_toolsMutex);
        report = m_tools;
    }
    ImDrawList *list = ImGui::GetWindowDrawList();
    if (!report || m_checkingTools.load()) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        spinner(list, ImVec2(at.x + em(0.7f), at.y + em(0.7f)), em(0.55f), m_time, palette::kGold);
        ImGui::Dummy(ImVec2(em(1.6f), em(1.4f)));
        ImGui::SameLine();
        ImGui::TextUnformatted("Looking for the tools...");
        if (!report)
            return;
        ImGui::Dummy(ImVec2(0.0f, em(0.4f)));
    }
    if (report->ready()) {
        drawIcon(list, Icon::Check, ImGui::GetCursorScreenPos() + ImVec2(em(0.55f), em(0.55f)), em(1.1f),
                 palette::kGood);
        ImGui::Dummy(ImVec2(em(1.3f), 0.0f));
        ImGui::SameLine(0.0f, em(0.3f));
        ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kGood));
        ImGui::TextUnformatted("Everything the build needs is here.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
    } else {
        wrapped(palette::kText, report->instructions);
        if (!report->installCommand.empty()) {
            ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
            wrapped(palette::kTextMuted, "Copy this into a terminal:");
            commandWell(report->installCommand, m_copiedAt, m_time);
        }
        ImGui::BeginDisabled(m_checkingTools.load() || !m_live);
        ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
        if (darkButton("Check again##tools", Icon::Reset))
            checkToolsAsync();
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0.0f, em(0.5f)));
    }
    if (ImGui::BeginTable("##tools", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthFixed, em(2.0f));
        ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##version", ImGuiTableColumnFlags_WidthFixed, em(11.0f));
        const float row = em(1.9f);
        const float padding = ImGui::GetStyle().CellPadding.y;
        for (const ToolCheck &tool : report->tools) {
            const bool oneLine = tool.problem.empty();
            const float lift = oneLine ? std::max(0.0f, (row - ImGui::GetTextLineHeight()) * 0.5f - padding) : 0.0f;
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row);
            ImGui::TableNextColumn();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 center(at.x + em(0.9f), oneLine ? at.y - padding + row * 0.5f : at.y + em(1.05f));
            if (tool.usable())
                stageIcon(list, center, em(1.2f), StageStatus::Done, m_time);
            else if (tool.optional)
                drawIcon(list, Icon::Warning, center, em(1.1f), palette::kWarn);
            else
                stageIcon(list, center, em(1.2f), StageStatus::Failed, m_time);
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + lift);
            ImGui::TextUnformatted(tool.name.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kTextMuted));
            if (oneLine) {
                ImGui::SameLine(0.0f, em(0.7f));
                ImGui::TextUnformatted(tool.purpose.c_str());
            } else {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(tool.problem.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + lift);
            if (tool.found) {
                // Long distribution strings ("g++ (Debian 14.2.0-19)") end cleanly.
                std::string shown = tool.version.size() > 30 ? tool.version.substr(0, 29) + "..." : tool.version;
                ImGui::TextColored(palette::rgba(tool.problem.empty() ? palette::kText : palette::kWarn), "%s",
                                   shown.c_str());
            } else {
                ImGui::TextColored(palette::rgba(tool.optional ? palette::kWarn : palette::kBad), "Not found");
            }
        }
        ImGui::EndTable();
    }
}

void LauncherApp::drawOptionsPage() {
    title("Options", "Nothing here is final: change any of it at any time. The game's own options live in its "
                     "menu, once it starts.");
    sectionHeader("Files");
    settingRow("Game files", "The disc's files are copied here, and the translated code is built next to them.");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(m_config.workspace.c_str());
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(m_pipeline.running() || !m_live);
    if (darkButton("Change...##workspace", Icon::Folder))
        browseWorkspace();
    ImGui::EndDisabled();
    if (!m_hasPicker) {
        ImGui::SameLine(0.0f, em(0.6f));
        ImGui::PushItemWidth(em(22.0f));
        if (ImGui::InputText("##workspace-path", m_typedWorkspace, sizeof m_typedWorkspace,
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_config.workspace = m_typedWorkspace;
            if (m_live && m_persist)
                saveConfig(m_config);
        }
        ImGui::PopItemWidth();
    }
    sectionHeader("Build");
    settingRow("Compile jobs", "More build faster but take more memory. The first build copies about a gigabyte "
                               "and translates the game's code; later ones only redo what changed.");
    const int cores = std::max(1, SDL_GetCPUCount());
    int jobs = std::clamp(m_config.jobs, 1, cores);
    ImGui::SetNextItemWidth(em(16.0f));
    if (ImGui::SliderInt("##jobs", &jobs, 1, cores, "%d jobs")) {
        m_config.jobs = jobs;
        if (m_live && m_persist)
            saveConfig(m_config);
    }
}

void LauncherApp::drawBuildPage() {
    title("Build", "Everything happens on this computer, from your disc. You can keep using it meanwhile; closing "
                   "the launcher stops the build.");
    const auto stages = m_live ? m_pipeline.stages() : m_previewStages;
    const bool running =
        m_live ? m_pipeline.running()
               : std::any_of(stages.begin(), stages.end(),
                             [](const StageState &stage) { return stage.status == StageStatus::Running; });
    const std::string error = m_live ? m_pipeline.error() : m_previewError;

    const auto elapsed = [](const StageState &stage) {
        if (stage.status != StageStatus::Running || stage.startedAt < 0.0)
            return stage.seconds;
        return nowSeconds() - stage.startedAt;
    };
    // The whole build, weighted by how long each step usually takes.
    double overall = 0.0, spent = 0.0;
    for (size_t i = 0; i < kStageCount; ++i) {
        const StageState &stage = stages[i];
        if (stage.status == StageStatus::Done || stage.status == StageStatus::Skipped)
            overall += kStageWeight[i];
        else if (stage.status == StageStatus::Running && stage.progress > 0.0)
            overall += kStageWeight[i] * stage.progress;
        spent += elapsed(stage);
    }
    const bool started = std::any_of(stages.begin(), stages.end(), [](const StageState &stage) {
        return stage.status != StageStatus::Waiting;
    });
    if (started) {
        char line[96];
        std::snprintf(line, sizeof line, "%.0f%%", overall * 100.0);
        pushFont(g_fonts.bold24);
        ImGui::TextUnformatted(line);
        popFont();
        if (running && spent >= 60.0) {
            ImGui::SameLine(0.0f, em(0.8f));
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kTextMuted));
            ImGui::Text("%s so far", duration(spent).c_str());
            ImGui::PopStyleColor();
        }
        progressBar(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x, overall,
                    em(0.7f), m_time);
        ImGui::Dummy(ImVec2(0.0f, em(0.7f)));
    }

    // One line a step: its state, its name, and what it is doing or took.
    ImDrawList *list = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < kStageCount; ++i) {
        const StageState &stage = stages[i];
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float right = at.x + ImGui::GetContentRegionAvail().x - em(0.3f);
        stageIcon(list, ImVec2(at.x + em(0.65f), at.y + ImGui::GetTextLineHeight() * 0.5f), em(1.15f), stage.status,
                  m_time);
        ImGui::SetCursorScreenPos(ImVec2(at.x + em(1.8f), at.y));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              palette::rgba(stage.status == StageStatus::Waiting ? palette::kTextMuted : palette::kText));
        ImGui::TextUnformatted(stageTitle(static_cast<Stage>(i)));
        ImGui::PopStyleColor();
        std::string detail = stage.detail;
        if (stage.status == StageStatus::Running && elapsed(stage) >= 60.0)
            detail += (detail.empty() ? "" : "  ") + duration(elapsed(stage));
        else if (stage.status != StageStatus::Running && stage.seconds >= 1.0)
            detail += (detail.empty() ? "" : "  ") + duration(stage.seconds);
        if (!detail.empty()) {
            const float detailWidth = ImGui::CalcTextSize(detail.c_str()).x;
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(std::max(ImGui::GetCursorScreenPos().x, right - detailWidth), at.y));
            ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kTextMuted));
            ImGui::TextUnformatted(detail.c_str());
            ImGui::PopStyleColor();
        }
        if (stage.status == StageStatus::Running) {
            ImGui::SetCursorScreenPos(ImVec2(at.x + em(1.8f), ImGui::GetCursorScreenPos().y + em(0.1f)));
            progressBar(list, ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x - em(1.8f), stage.progress,
                        em(0.32f), m_time);
        }
        if (!stage.note.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(at.x + em(1.8f), ImGui::GetCursorScreenPos().y + em(0.15f)));
            drawIcon(list, Icon::Info, ImGui::GetCursorScreenPos() + ImVec2(em(0.55f), em(0.55f)), em(1.1f),
                     palette::kGold);
            ImGui::Dummy(ImVec2(em(1.3f), 0.0f));
            ImGui::SameLine(0.0f, em(0.3f));
            wrapped(palette::kText, stage.note);
        }
        ImGui::SetCursorScreenPos(ImVec2(at.x, ImGui::GetCursorScreenPos().y + em(0.15f)));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }

    if (!error.empty()) {
        ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
        drawIcon(list, Icon::Warning, ImGui::GetCursorScreenPos() + ImVec2(em(0.6f), em(0.6f)), em(1.2f),
                 palette::kBad);
        ImGui::Dummy(ImVec2(em(1.5f), 0.0f));
        ImGui::SameLine(0.0f, em(0.3f));
        wrapped(palette::kBad, error);
    }
    if (m_showLog || (!m_live && !m_previewLog.empty())) {
        ImGui::Dummy(ImVec2(0.0f, em(0.3f)));
        // The stopped log becomes one selectable block; the live one scrolls.
        const bool copiedLog = m_time - m_logCopiedAt < 2.0;
        if (darkButton(copiedLog ? "Copied" : "Copy log##build", Icon::Copy) && m_live) {
            SDL_SetClipboardText(bugReport().c_str());
            m_logCopiedAt = m_time;
        }
        ImGui::SameLine(0.0f, em(0.6f));
        if (darkButton("Open log folder##build", Icon::Folder) && m_live)
            SDL_OpenURL(("file://" + std::filesystem::path(m_config.workspace).string()).c_str());
        ImGui::SameLine(0.0f, em(0.6f));
        if (darkButton("Report a problem##build", Icon::Link))
            SDL_OpenURL("https://github.com/rakmahefa/tagforcerecomp-preview/issues/new");
        ImGui::Dummy(ImVec2(0.0f, em(0.2f)));
        // Enough for the page above to fit: the log takes what is left, at least this.
        const float height = std::max(em(6.0f), ImGui::GetContentRegionAvail().y - em(0.2f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(4, 6, 18, 190));
        ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kTextMuted));
        if (running) {
            const std::vector<std::string> lines = m_live ? m_pipeline.log(300) : m_previewLog;
            ImGui::BeginChild("##log", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
            ImGui::PushTextWrapPos(0.0f);
            for (const std::string &logLine : lines)
                ImGui::TextUnformatted(logLine.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - em(2.0f))
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        } else {
            const uint64_t version = m_live ? m_pipeline.logVersion() : 0u;
            if (!m_logActive && (version != m_logTextVersion || m_logText.empty())) {
                m_logText.clear();
                for (const std::string &logLine : m_live ? m_pipeline.log(1000) : m_previewLog)
                    m_logText += logLine + '\n';
                m_logTextVersion = version;
            }
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, ImGui::GetStyle().ChildBorderSize);
            ImGui::InputTextMultiline("##log-stopped", m_logText.data(), m_logText.size() + 1u,
                                      ImVec2(-FLT_MIN, height),
                                      ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoHorizontalScroll);
            ImGui::PopStyleVar();
            m_logActive = ImGui::IsItemActive();
        }
        ImGui::PopStyleColor(2);
        if (m_revealLog) {
            ImGui::SetScrollHereY(1.0f);
            m_revealLog = false;
        }
    }
}

void LauncherApp::drawPlayPage() {
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList *list = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(0.0f, em(1.5f)));
    const ImVec2 starAt = ImGui::GetCursorScreenPos();
    drawIcon(list, Icon::Star, ImVec2(starAt.x + width * 0.5f, starAt.y + em(2.2f)), em(4.2f), palette::kGold);
    ImGui::Dummy(ImVec2(0.0f, em(4.6f)));
    const auto centered = [&](const char *text, ImU32 tint, ImFont *font) {
        pushFont(font);
        const float w = ImGui::CalcTextSize(text).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (width - w) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(tint));
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
        popFont();
    };
    centered(m_built || !m_live ? "Ready to play" : "Ready when the runtime is", palette::kText, g_fonts.bold34);
    centered(m_built || !m_live ? "Built on this computer from your disc." : "Your disc, translated and waiting.",
             palette::kTextMuted, g_fonts.regular);
    ImGui::Dummy(ImVec2(0.0f, em(1.2f)));
    if (m_live && !m_built) {
        // This preview's honest limit, said plainly.
        ImGui::PushStyleColor(ImGuiCol_Text, palette::rgba(palette::kGold));
        drawIcon(list, Icon::Info, ImGui::GetCursorScreenPos() + ImVec2(em(0.6f), em(0.6f)), em(1.2f), palette::kGold);
        ImGui::Dummy(ImVec2(em(1.5f), 0.0f));
        ImGui::SameLine(0.0f, em(0.3f));
        wrapped(palette::kText,
                "The Windows runtime does not build on Linux yet. This preview copies your game files and translates "
                "the game's code into " +
                    std::filesystem::path(m_config.workspace).string() +
                    "; Play starts the game the moment a Linux build of TagForceRecomp sits beside the launcher.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, em(0.8f)));
    }
    // The big button.
    const float playWidth = em(14.0f), playHeight = em(3.4f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - playWidth) * 0.5f);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##play-big", ImVec2(playWidth, playHeight));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 max(min.x + playWidth, min.y + playHeight);
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(m_time) * 2.4f);
    // A soft halo that breathes with the button, never a second button.
    list->AddRectFilled(ImVec2(min.x - 6, min.y - 6), ImVec2(max.x + 6, max.y + 6),
                        palette::withAlpha(palette::kGold, static_cast<int>(10 + 12 * pulse)), playHeight * 0.5f + 6);
    list->AddRectFilledMultiColor(min, max, hovered ? IM_COL32(252, 218, 126, 255) : palette::kGold,
                                  hovered ? IM_COL32(252, 218, 126, 255) : palette::kGold, palette::kGoldDeep,
                                  palette::kGoldDeep);
    list->AddRect(min, max, IM_COL32(255, 246, 216, 220), playHeight * 0.5f, 0, 1.6f);
    pushFont(g_fonts.bold28);
    const ImVec2 label = ImGui::CalcTextSize("Play");
    const float iconSize = em(1.3f);
    const float total = iconSize + em(0.5f) + label.x;
    const float x0 = min.x + (playWidth - total) * 0.5f;
    drawIcon(list, Icon::Play, ImVec2(x0 + iconSize * 0.5f, min.y + playHeight * 0.5f), iconSize,
             IM_COL32(42, 28, 2, 255));
    list->AddText(g_fonts.bold28, g_fonts.bold28 ? g_fonts.bold28->FontSize : 0.0f,
                  ImVec2(x0 + iconSize + em(0.5f), min.y + (playHeight - label.y) * 0.5f), IM_COL32(42, 28, 2, 255),
                  "Play");
    popFont();
    if (pressed)
        launchGame();
    if (!m_launchError.empty()) {
        ImGui::Dummy(ImVec2(0.0f, em(0.6f)));
        wrapped(palette::kBad, m_launchError);
    }

    ImGui::Dummy(ImVec2(0.0f, em(1.6f)));
    const float row = buttonWidth("Game files##play") + em(0.6f) + buttonWidth("Options##play") + em(0.6f) +
                      buttonWidth("Rebuild##play");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (width - row) * 0.5f));
    if (darkButton("Game files##play", Icon::Folder) && m_live)
        SDL_OpenURL(("file://" + std::filesystem::path(m_config.workspace).string()).c_str());
    ImGui::SameLine(0.0f, em(0.6f));
    if (darkButton("Options##play", Icon::Sliders))
        goTo(Page::Options);
    ImGui::SameLine(0.0f, em(0.6f));
    if (darkButton("Rebuild##play", Icon::Hammer))
        m_page = Page::Build;
    ImGui::Dummy(ImVec2(0.0f, em(1.6f)));
    centered("Nothing from your disc is uploaded: everything stays on this computer.", palette::kTextMuted,
             g_fonts.regular);
}

} // namespace tf::launcher
