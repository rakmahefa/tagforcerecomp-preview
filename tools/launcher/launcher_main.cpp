// launcher_main.cpp - the Linux launcher's window and command line.
//
//   tagforce-launcher [--repo DIR] [--disc ISO] [--workspace DIR] [--jobs N]
//                     [--build | --preview DIR | --smoke | --screenshot FILE]
//                     [--check-disc ISO]
//
// The window: SDL2 + Dear ImGui over OpenGL (software GL is enough). --preview
// draws the eight fixed pages into PNGs for the documentation, --smoke draws
// them without saving, --screenshot saves the live window, --build runs the
// pipeline with no window, --check-disc recognises an ISO and exits.
#include "launcher.h"
#include "launcher_app.h"
#include "launcher_iso.h"
#include "launcher_pipeline.h"
#include "launcher_png.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace tf::launcher;

namespace {

volatile std::sig_atomic_t g_interrupted = 0;
void onInterrupt(int) { g_interrupted = 1; }

// glReadPixels, resolved from the context (no GL headers needed to build).
using GlReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void *);
GlReadPixels g_glReadPixels = nullptr;

// A checkout of this project: given, above the executable, or above the cwd.
bool isRepo(const fs::path &dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "tools/translator/recomp.py", ec) &&
           fs::is_regular_file(dir / "runtime/core/psp_rt.h", ec);
}

std::optional<fs::path> findRepo(const std::string &given) {
    if (!given.empty())
        return isRepo(fs::path(given)) ? std::optional(fs::path(given)) : std::nullopt;
    std::vector<fs::path> starts;
    if (const char *base = SDL_GetBasePath())
        starts.push_back(fs::path(base));
    std::error_code ec;
    starts.push_back(fs::current_path(ec));
    for (fs::path dir : starts) {
        for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
            if (isRepo(dir))
                return fs::weakly_canonical(dir, ec);
            if (dir == dir.parent_path())
                break;
            dir = dir.parent_path();
        }
    }
    return std::nullopt;
}

// The fonts, from the release, the checkout, or the system.
bool loadFonts() {
    std::vector<fs::path> candidates;
    if (const char *given = SDL_getenv("TF_LAUNCHER_FONTS"))
        candidates.push_back(fs::path(given));
    if (const char *base = SDL_GetBasePath()) {
        candidates.push_back(fs::path(base) / "payload/source/tools/launcher/fonts");
        candidates.push_back(fs::path(base).parent_path() / "tools/launcher/fonts");
        candidates.push_back(fs::path(base) / "fonts");
    }
    candidates.push_back("/usr/share/fonts/truetype/liberation");
    ImGuiIO &io = ImGui::GetIO();
    for (const fs::path &dir : candidates) {
        std::error_code ec;
        const fs::path regular = dir / "LiberationSans-Regular.ttf";
        const fs::path bold = dir / "LiberationSans-Bold.ttf";
        if (!fs::is_regular_file(regular, ec) || !fs::is_regular_file(bold, ec))
            continue;
        g_fonts.regular = io.Fonts->AddFontFromFileTTF(regular.string().c_str(), 18.0f);
        g_fonts.bold = io.Fonts->AddFontFromFileTTF(bold.string().c_str(), 18.0f);
        g_fonts.bold22 = io.Fonts->AddFontFromFileTTF(bold.string().c_str(), 22.0f);
        g_fonts.bold24 = io.Fonts->AddFontFromFileTTF(bold.string().c_str(), 24.0f);
        g_fonts.bold28 = io.Fonts->AddFontFromFileTTF(bold.string().c_str(), 28.0f);
        g_fonts.bold34 = io.Fonts->AddFontFromFileTTF(bold.string().c_str(), 34.0f);
        if (g_fonts.regular)
            return true;
    }
    std::fprintf(stderr, "launcher: Liberation Sans not found; using the default font\n");
    return false;
}

void applyStyle() {
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(em(0.8f), em(0.55f));
    s.FramePadding = ImVec2(em(0.55f), em(0.4f));
    s.ItemSpacing = ImVec2(em(0.45f), em(0.4f));
    s.ItemInnerSpacing = ImVec2(em(0.3f), em(0.2f));
    s.ScrollbarSize = em(0.8f);
    s.GrabMinSize = em(0.7f);
    s.WindowRounding = 0.0f;
    s.ChildRounding = em(0.4f);
    s.FrameRounding = em(0.3f);
    s.GrabRounding = em(0.35f);
    s.PopupRounding = em(0.35f);
    s.ScrollbarRounding = em(0.4f);
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    s.SeparatorTextBorderSize = em(0.15f);

    auto rgba = [](ImU32 c) { return palette::rgba(c); };
    ImGui::StyleColorsDark();
    s.Colors[ImGuiCol_Text] = rgba(palette::kText);
    s.Colors[ImGuiCol_TextDisabled] = rgba(palette::kTextMuted);
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.065f, 0.16f, 1.0f);
    s.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    s.Colors[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.065f, 0.16f, 0.96f);
    s.Colors[ImGuiCol_Border] = rgba(palette::kBorderInner);
    s.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    s.Colors[ImGuiCol_Separator] = rgba(palette::kBorderInner);
    s.Colors[ImGuiCol_SeparatorHovered] = rgba(palette::kGold);
    s.Colors[ImGuiCol_SeparatorActive] = rgba(palette::kGold);
    s.Colors[ImGuiCol_FrameBg] = rgba(palette::kFill);
    s.Colors[ImGuiCol_FrameBgHovered] = rgba(palette::kFillStrong);
    s.Colors[ImGuiCol_FrameBgActive] = rgba(palette::kFillStrong);
    s.Colors[ImGuiCol_TitleBg] = rgba(palette::kFill);
    s.Colors[ImGuiCol_TitleBgActive] = rgba(palette::kFillStrong);
    s.Colors[ImGuiCol_TitleBgCollapsed] = rgba(palette::kFill);
    s.Colors[ImGuiCol_MenuBarBg] = rgba(palette::kFill);
    s.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.03f, 0.04f, 0.12f, 0.6f);
    s.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.12f, 0.14f, 0.30f, 1.0f);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = rgba(palette::kFillStrong);
    s.Colors[ImGuiCol_ScrollbarGrabActive] = rgba(palette::kGoldDeep);
    s.Colors[ImGuiCol_CheckMark] = rgba(palette::kGold);
    s.Colors[ImGuiCol_SliderGrab] = rgba(palette::kGold);
    s.Colors[ImGuiCol_SliderGrabActive] = rgba(palette::kGoldSoft);
    s.Colors[ImGuiCol_Button] = rgba(palette::kFill);
    s.Colors[ImGuiCol_ButtonHovered] = rgba(palette::kFillStrong);
    s.Colors[ImGuiCol_ButtonActive] = rgba(palette::kFillStrong);
    s.Colors[ImGuiCol_Header] = rgba(palette::kFill);
    s.Colors[ImGuiCol_HeaderHovered] = rgba(palette::kFillStrong);
    s.Colors[ImGuiCol_HeaderActive] = rgba(palette::kFillStrong);
    s.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    s.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
    s.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.94f, 0.78f, 0.36f, 0.35f);
    s.Colors[ImGuiCol_DragDropTarget] = rgba(palette::kGold);
    s.Colors[ImGuiCol_NavCursor] = rgba(palette::kGold);
    s.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
    s.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.8f, 0.8f, 0.8f, 0.2f);
    s.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.08f, 0.09f, 0.2f, 0.6f);
}

// A gold star on night blue, for the window's icon.
void setWindowIcon(SDL_Window *window) {
    const int size = 32;
    std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4, 0);
    const float cx = size * 0.5f, cy = size * 0.5f, outer = size * 0.42f, inner = size * 0.17f;
    float star[10][2];
    for (int i = 0; i < 10; ++i) {
        const float radius = (i % 2 == 0) ? outer : inner;
        const float angle = -M_PI / 2.0f + static_cast<float>(i) * (M_PI / 5.0f);
        star[i][0] = cx + std::cos(angle) * radius;
        star[i][1] = cy + std::sin(angle) * radius;
    }
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            // Even-odd ray casting against the star polygon.
            bool inside = false;
            for (int i = 0, j = 9; i < 10; j = i++) {
                if ((star[i][1] > y) != (star[j][1] > y) &&
                    x < (star[j][0] - star[i][0]) * (y - star[i][1]) / (star[j][1] - star[i][1]) + star[i][0])
                    inside = !inside;
            }
            unsigned char *px = &pixels[(y * size + x) * 4];
            if (inside) {
                px[0] = 240;
                px[1] = 200;
                px[2] = 92;
            } else {
                px[0] = 13;
                px[1] = 17;
                px[2] = 40;
            }
            px[3] = 255;
        }
    }
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (surface) {
        SDL_memcpy(surface->pixels, pixels.data(), pixels.size());
        SDL_SetWindowIcon(window, surface);
        SDL_FreeSurface(surface);
    }
}

// Shown when the launcher is not beside a payload or a checkout.
void drawNoRepo() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em(30.0f), 0.0f));
    ImGui::Begin("TagForceRecomp", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("The launcher has to sit in the TagForceRecomp folder, the one with the payload folder "
                           "and the runtime sources, or be started with --repo and that folder's path.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0.0f, em(0.6f)));
    if (ImGui::Button("Quit")) {
        SDL_Event quit;
        SDL_zero(quit);
        quit.type = SDL_QUIT;
        SDL_PushEvent(&quit);
    }
    ImGui::End();
}

// The window's build, without the window: progress goes to stdout.
int headlessBuild(const std::optional<Payload> &payload, const std::optional<fs::path> &repo,
                  const LauncherConfig &config) {
    if (config.disc.empty()) {
        std::fprintf(stderr, "--build needs --disc\n");
        return 2;
    }
    Pipeline pipeline;
    PipelineOptions options;
    options.workspace = !config.workspace.empty() ? config.workspace
                         : payload ? defaultGamesFolder()
                                   : repo->parent_path().string();
    options.source = payload ? payload->source : *repo;
    options.disc = config.disc;
    options.jobs = config.jobs > 0 ? config.jobs : defaultJobs();
    std::printf("[launcher] building into %s with %d jobs%s\n", options.workspace.c_str(), options.jobs,
                payload ? (", from release " + payload->version).c_str() : "");
    pipeline.start(options);
    std::array<StageState, kStageCount> shown{};
    double lastProgress = nowSeconds() - 60.0;
    do {
        SDL_Delay(500);
        if (g_interrupted) {
            g_interrupted = 0;
            std::printf("[launcher] stopping\n");
            pipeline.cancel();
        }
        const auto stages = pipeline.stages();
        for (size_t i = 0; i < kStageCount; ++i) {
            const StageState &stage = stages[i];
            const bool changed = stage.status != shown[i].status;
            const bool progress = stage.status == StageStatus::Running && nowSeconds() - lastProgress > 30.0;
            if (!changed && !progress)
                continue;
            std::printf("[launcher] %s: %s", stageTitle(static_cast<Stage>(i)), statusName(stage.status));
            if (!stage.detail.empty())
                std::printf(", %s", stage.detail.c_str());
            if (stage.status == StageStatus::Running && stage.progress >= 0.0)
                std::printf(", %.0f%%", stage.progress * 100.0);
            if (stage.status == StageStatus::Done)
                std::printf(" in %.0f s", stage.seconds);
            std::printf("\n");
            std::fflush(stdout);
            shown[i] = stage;
        }
    } while (pipeline.running());
    if (pipeline.succeeded()) {
        std::printf("[launcher] ready\n");
        return 0;
    }
    if (pipeline.stoppedAtGap()) {
        std::printf("[launcher] stopped before the runtime: this payload has no tools/build_linux.sh; the game files "
                    "and translated code are ready in %s\n",
                    options.workspace.c_str());
        return 0;
    }
    std::printf("[launcher] FAILED: %s\n", pipeline.error().c_str());
    for (const std::string &line : pipeline.log(40))
        std::printf("  %s\n", line.c_str());
    return 1;
}

} // namespace

int main(int argc, char **argv) {
    std::string repoArg, previewDir, snapshotFile;
    LauncherConfig overrides;
    bool smoke = false, build = false, checkDisc = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--repo" && i + 1 < argc)
            repoArg = argv[++i];
        else if (arg == "--disc" && i + 1 < argc)
            overrides.disc = argv[++i];
        else if (arg == "--workspace" && i + 1 < argc)
            overrides.workspace = argv[++i];
        else if (arg == "--jobs" && i + 1 < argc)
            overrides.jobs = std::atoi(argv[++i]);
        else if (arg == "--build")
            build = true;
        else if (arg == "--preview" && i + 1 < argc)
            previewDir = argv[++i];
        else if (arg == "--smoke")
            smoke = true;
        else if (arg == "--screenshot" && i + 1 < argc)
            snapshotFile = argv[++i];
        else if (arg == "--check-disc" && i + 1 < argc) {
            overrides.disc = argv[++i];
            checkDisc = true;
        } else {
            std::fprintf(stderr,
                         "usage: tagforce-launcher [--repo DIR] [--disc ISO] [--workspace DIR] [--jobs N]\n"
                         "                         [--build | --preview DIR | --smoke | --screenshot FILE |\n"
                         "                          --check-disc ISO]\n");
            return arg == "--help" ? 0 : 2;
        }
    }

    // Recognise a disc and leave: the testable path of the Disc page.
    if (checkDisc) {
        const DiscInfo disc = inspectDisc(fs::path(overrides.disc));
        std::printf("disc: %s\n", overrides.disc.c_str());
        std::printf("game id: %s\n", disc.gameId.empty() ? "(none)" : disc.gameId.c_str());
        std::printf("size: %s\n", humanSize(disc.size).c_str());
        std::printf("supported: %s\n", disc.supported ? "yes" : "no");
        if (!disc.error.empty())
            std::printf("%s\n", disc.error.c_str());
        return disc.supported ? 0 : 1;
    }

    const bool previewing = smoke || !previewDir.empty();
    const bool snapshot = !snapshotFile.empty();
    std::signal(SIGINT, onInterrupt);
    std::signal(SIGTERM, onInterrupt);

    // A release builds from its payload; --repo picks a checkout over it.
    const std::optional<Payload> payload = repoArg.empty() ? findPayload() : std::nullopt;

    if (build) {
        SDL_Init(0);
        const std::optional<fs::path> repo = payload ? std::nullopt : findRepo(repoArg);
        if (!repo && !payload) {
            std::fprintf(stderr, "not inside a Tag Force Recomp source tree; pass --repo\n");
            return 2;
        }
        const int status = headlessBuild(payload, repo, overrides);
        SDL_Quit();
        return status;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return previewing || snapshot ? 77 : 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    SDL_WindowFlags flags = static_cast<SDL_WindowFlags>(
        SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI |
        (previewing ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE));
    SDL_Window *window = SDL_CreateWindow("TagForceRecomp Launcher", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          kWindowWidth, kWindowHeight, flags);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!window || !context) {
        std::fprintf(stderr, "no window or GL context: %s\n", SDL_GetError());
        return previewing || snapshot ? 77 : 1;
    }
    SDL_GL_MakeCurrent(window, context);
    SDL_GL_SetSwapInterval(previewing ? 0 : 1);
    if (!previewing)
        SDL_SetWindowMinimumSize(window, 900, 640);
    setWindowIcon(window);
    g_glReadPixels = reinterpret_cast<GlReadPixels>(SDL_GL_GetProcAddress("glReadPixels"));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    loadFonts();
    applyStyle();
    ImGui_ImplSDL2_InitForOpenGL(window, context);
    ImGui_ImplOpenGL3_Init("#version 130");

    std::optional<fs::path> repo = payload ? std::nullopt : findRepo(repoArg);
    if (!repo && !payload && (previewing || snapshot))
        repo = fs::current_path();
    std::optional<LauncherApp> app;
    // Only a player's window saves: screenshots must leave the ini alone.
    if (repo || payload)
        app.emplace(repo.value_or(fs::path()), payload, window, overrides, !previewing);

    // One frame; `capture` saves it as PNG when set.
    const auto frame = [&](const char *capture) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        if (app)
            app->draw();
        else
            drawNoRepo();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (capture && g_glReadPixels) {
            int width = 0, height = 0;
            SDL_GL_GetDrawableSize(window, &width, &height);
            std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
            g_glReadPixels(0, 0, width, height, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, pixels.data());
            std::error_code ec;
            const fs::path path(capture);
            if (!path.parent_path().empty())
                fs::create_directories(path.parent_path(), ec);
            if (!savePng(capture, width, height, pixels.data())) {
                std::fprintf(stderr, "FAIL: could not save %s\n", capture);
                return false;
            }
            std::printf("wrote %s\n", capture);
        }
        SDL_GL_SwapWindow(window);
        return true;
    };
    const auto settle = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            SDL_Event event;
            while (SDL_PollEvent(&event))
                ImGui_ImplSDL2_ProcessEvent(&event);
            frame(nullptr);
        }
    };

    int status = 0;
    if (previewing) {
        for (int index = 0; index < static_cast<int>(Preview::Count); ++index) {
            const auto preview = static_cast<Preview>(index);
            app->showPreview(preview);
            settle(12);
            if (!previewDir.empty()) {
                const std::string name = previewName(preview);
                if (!frame((fs::path(previewDir) / (name + ".png")).string().c_str()))
                    status = 1;
            }
        }
        if (status == 0 && previewDir.empty())
            std::printf("PASS: launcher preview drew every page\n");
        else if (status == 0)
            std::printf("PASS: launcher preview drew every page into %s\n", previewDir.c_str());
    } else if (snapshot) {
        int frameNo = 0;
        bool running = true;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
                    running = false;
                if (app)
                    app->handleEvent(event);
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(50);
                continue;
            }
            frame(frameNo == 90 ? snapshotFile.c_str() : nullptr);
            if (++frameNo > 100)
                running = false;
        }
    } else {
        bool running = true;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
                     event.window.windowID == SDL_GetWindowID(window)))
                    running = false;
                if (app)
                    app->handleEvent(event);
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                SDL_Delay(50);
                continue;
            }
            frame(nullptr);
        }
    }

    app.reset();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
