// launcher_app.h - the launcher's window: five pages from the disc to Play.
#pragma once

#include "launcher.h"
#include "launcher_pipeline.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace tf::launcher {

// Fixed states for the screenshots, drawn with no live work behind them
// (--preview): the same idea as the reference launcher's documentation pages.
enum class Preview : uint8_t {
    DiscEmpty, DiscReady, ToolsReady, ToolsMissing, Options, Building, BuildFailed, Play, Count
};
const char *previewName(Preview preview);

// What the system's file picker answered; it runs on its own thread.
struct DialogPicks {
    std::mutex mutex;
    std::optional<std::string> disc, workspace;
    bool browsing = false;
};

class LauncherApp {
public:
    // From a checkout, `repo` is it; from a release, `payload` carries the
    // source. `overrides` replaces the remembered disc, folder and jobs;
    // without `persist`, nothing is written (screenshots, tests).
    LauncherApp(std::filesystem::path repo, std::optional<Payload> payload, SDL_Window *window,
                const LauncherConfig &overrides = {}, bool persist = true);
    ~LauncherApp();

    void handleEvent(const SDL_Event &event);
    void draw();                // inside an ImGui frame
    void showPreview(Preview preview);
    void setPage(Page page) { m_page = page; }
    bool checkingTools() const { return m_checkingTools.load(); }

private:
    // Drawing.
    void drawBackground();
    void drawSidebar();
    void drawFooter();
    void drawDiscPage();
    void drawToolsPage();
    void drawOptionsPage();
    void drawBuildPage();
    void drawPlayPage();
    // Actions.
    void setDisc(const std::string &path);
    void checkToolsAsync();
    void startBuild();
    void launchGame();
    void browseDisc();
    void browseWorkspace();
    bool pageDone(Page page) const;
    bool pageReachable(Page page) const;
    void goTo(Page page) {
        if (pageReachable(page))
            m_page = page;
    }
    // Places.
    std::filesystem::path installDir() const; // where the launcher (and the game) lives
    std::filesystem::path sourceRoot() const; // payload/source, or the checkout
    std::filesystem::path gamePath() const;   // the Linux runtime, once it exists
    std::string bugReport() const;            // what to paste into an issue

    std::filesystem::path m_repo; // the checkout, or empty from a release
    std::optional<Payload> m_payload;
    SDL_Window *m_window = nullptr;
    LauncherConfig m_config;
    Page m_page = Page::Disc;
    bool m_live = true;
    bool m_persist = true;
    double m_time = 0.0;
    bool m_hasPicker = false; // zenity is there
    char m_typedPath[1024] = {};
    char m_typedWorkspace[1024] = {};

    // Disc page.
    std::optional<DiscInfo> m_disc;
    bool m_dragging = false;
    std::shared_ptr<DialogPicks> m_picks = std::make_shared<DialogPicks>();

    // Tools page, filled by a worker.
    mutable std::mutex m_toolsMutex;
    std::optional<ToolReport> m_tools;
    std::atomic<bool> m_checkingTools{false};
    std::thread m_toolsThread;
    double m_copiedAt = -10.0; // the "Copied" flash on the install command

    // Build page.
    Pipeline m_pipeline;
    bool m_showLog = false;
    bool m_revealLog = false;
    bool m_buildWasRunning = false;
    std::string m_logText; // the stopped log as one selectable block
    uint64_t m_logTextVersion = UINT64_MAX;
    bool m_logActive = false;
    double m_logCopiedAt = -10.0;
    std::array<StageState, kStageCount> m_previewStages{};
    std::string m_previewError;
    std::vector<std::string> m_previewLog;

    // Play page.
    std::string m_launchError;
    mutable bool m_built = false;
    mutable double m_builtCheckedAt = -1.0;
};

} // namespace tf::launcher
