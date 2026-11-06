// launcher_pipeline.cpp - runs the build's stages as child processes.
//
//   Check the disc        the ISO is read again (game id, size) - no child
//   Unpack the source     payload/source (or the checkout) -> <workspace>/source
//   Copy the game files   python3 tools/game/extrair_jogo.py <iso> <workspace>/jogo
//   Translate the code    TF_JOGO=<workspace>/jogo python3 tools/translator/build_modules.py
//   Compile the runtime   JOBS=n sh tools/build_linux.sh (the [n/N] lines are the progress bar)
//
// The last stage leaves the game in <workspace>/source: the executable, TinyCC and the
// header the generated code includes, beside each other, exactly where Play looks for them.
#include "launcher_pipeline.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tf::launcher {
namespace {

// A quoted, single shell word. Paths live in the command line, never apart.
std::string q(const std::filesystem::path &path) { return shellQuote(path.string()); }

// Runs `sh -c command` in its own process group, streaming merged stdout and
// stderr line by line (a carriage return starts a new line too: the extractor
// paints its progress with \r). Returns the exit code, or -1 when cancelled.
int runCommand(const std::string &command, const std::atomic<bool> &cancel, std::atomic<pid_t> &child,
               const std::function<void(const std::string &)> &onLine) {
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0); // its own group, so a cancel takes the whole tree
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    child = pid;
    std::string pending;
    char buffer[4096];
    for (;;) {
        ssize_t got;
        while ((got = read(fds[0], buffer, sizeof buffer)) > 0) {
            for (ssize_t i = 0; i < got; ++i) {
                if (buffer[i] == '\n' || buffer[i] == '\r') {
                    if (!pending.empty())
                        onLine(pending);
                    pending.clear();
                } else {
                    pending += buffer[i];
                }
            }
        }
        if (got == 0)
            break; // eof: the child is done writing
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0)
            break;
    }
    close(fds[0]);
    for (;;) {
        if (cancel.load()) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
        }
        int status = 0;
        const pid_t done = waitpid(pid, &status, 0);
        if (done == pid) {
            child = -1;
            if (cancel.load())
                return -1;
            if (WIFEXITED(status))
                return WEXITSTATUS(status);
            if (WIFSIGNALED(status))
                return 128 + WTERMSIG(status);
            return -1;
        }
        if (done < 0 && errno == EINTR)
            continue;
        child = -1;
        return -1;
    }
}

// "[extrair]  42% PSP_GAME/..." -> 0.42, or -1 when the line has no percent.
double percentOf(const std::string &line) {
    const size_t at = line.find('%');
    if (at == std::string::npos || at == 0)
        return -1.0;
    size_t start = at;
    while (start > 0 && isdigit(static_cast<unsigned char>(line[start - 1])))
        --start;
    if (start == at)
        return -1.0;
    return std::atof(line.c_str() + start) / 100.0;
}

} // namespace

double nowSeconds() {
    static const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
}

const char *stageTitle(Stage stage) {
    switch (stage) {
    case Stage::CheckDisc: return "Check the disc";
    case Stage::UnpackSource: return "Unpack the source";
    case Stage::CopyFiles: return "Copy the game files";
    case Stage::TranslateCode: return "Translate the game's code";
    case Stage::CompileRuntime: return "Compile the runtime";
    case Stage::Count: break;
    }
    return "?";
}

Pipeline::~Pipeline() {
    m_cancel = true;
    if (m_child > 0) {
        kill(-m_child, SIGKILL);
        kill(m_child, SIGKILL);
    }
    if (m_thread.joinable())
        m_thread.join();
}

std::string Pipeline::error() const {
    std::lock_guard lock(m_mutex);
    return m_error;
}

std::array<StageState, kStageCount> Pipeline::stages() const {
    std::lock_guard lock(m_mutex);
    return m_stages;
}

std::vector<std::string> Pipeline::log(size_t limit) const {
    std::lock_guard lock(m_mutex);
    std::vector<std::string> tail;
    const size_t first = m_log.size() > limit ? m_log.size() - limit : 0;
    for (size_t i = first; i < m_log.size(); ++i)
        tail.push_back(m_log[i]);
    return tail;
}

void Pipeline::start(const PipelineOptions &options) {
    if (m_running.exchange(true))
        return; // already building
    if (m_thread.joinable())
        m_thread.join();
    {
        std::lock_guard lock(m_mutex);
        m_stages = {};
        m_log.clear();
        m_logVersion++;
        m_error.clear();
        m_succeeded = false;
    }
    m_cancel = false;
    m_started = true;
    m_gap = false;
    m_thread = std::thread([this, options] { run(options); });
}

void Pipeline::cancel() {
    m_cancel = true;
    if (m_child > 0) {
        kill(-m_child, SIGTERM);
        kill(m_child, SIGTERM);
    }
}

void Pipeline::run(PipelineOptions options) {
    const auto line = [&](const std::string &text) {
        std::lock_guard lock(m_mutex);
        if (m_log.size() >= 600)
            m_log.pop_front();
        m_log.push_back(text);
        m_logVersion++;
    };
    const auto beginStage = [&](Stage stage, const std::string &detail = {}) {
        std::lock_guard lock(m_mutex);
        m_stages[static_cast<size_t>(stage)].status = StageStatus::Running;
        m_stages[static_cast<size_t>(stage)].startedAt = nowSeconds();
        m_stages[static_cast<size_t>(stage)].detail = detail;
    };
    const auto finishStage = [&](Stage stage, bool ok, const std::string &detail, const std::string &note = {}) {
        std::lock_guard lock(m_mutex);
        StageState &state = m_stages[static_cast<size_t>(stage)];
        state.status = ok ? StageStatus::Done : StageStatus::Failed;
        state.progress = ok ? 1.0 : state.progress;
        state.seconds = nowSeconds() - state.startedAt;
        state.detail = detail;
        if (!note.empty())
            state.note = note;
    };
    const auto setProgress = [&](Stage stage, double progress, const std::string &detail) {
        std::lock_guard lock(m_mutex);
        StageState &state = m_stages[static_cast<size_t>(stage)];
        state.progress = progress;
        state.detail = detail;
    };
    const auto fail = [&](const std::string &text) {
        std::lock_guard lock(m_mutex);
        m_error = text;
    };
    const auto shell = [&](const std::string &command, Stage stage,
                           const std::function<void(const std::string &)> &watch = {}) -> int {
        line("$ " + command);
        return runCommand(command, m_cancel, m_child, [&](const std::string &text) {
            line(text);
            if (watch)
                watch(text);
        });
    };
    const std::filesystem::path sourceCopy = options.workspace / "source";

    // 1. Check the disc: read it again, here and now.
    beginStage(Stage::CheckDisc);
    const DiscInfo disc = inspectDisc(options.disc);
    if (!disc.supported) {
        finishStage(Stage::CheckDisc, false, disc.gameId, disc.error);
        fail(disc.error);
        m_running = false;
        return;
    }
    finishStage(Stage::CheckDisc, true, disc.gameId + "  " + humanSize(disc.size));

    // 2. Unpack the source: a fresh copy of payload/source (or the checkout).
    beginStage(Stage::UnpackSource, "Copying the source tree");
    {
        const std::string command = "rm -rf " + q(sourceCopy) + " && mkdir -p " + q(options.workspace) +
                                    " && cp -a " + q(options.source) + "/. " + q(sourceCopy) + "/";
        const int code = shell(command, Stage::UnpackSource);
        if (m_cancel.load() || code != 0) {
            finishStage(Stage::UnpackSource, false, code == -1 ? "stopped" : "could not copy the source");
            if (!m_cancel.load())
                fail("Unpacking the source failed; the log shows why.");
            m_running = false;
            return;
        }
    }
    finishStage(Stage::UnpackSource, true, "into " + (options.workspace / "source").string());

    // 3. Copy the game files from the disc into <workspace>/jogo.
    beginStage(Stage::CopyFiles);
    {
        const std::string command = "python3 " + q(sourceCopy / "tools/game/extrair_jogo.py") + " " +
                                    q(options.disc) + " " + q(options.workspace / "jogo");
        int code;
        {
            double progress = -1.0;
            code = shell(command, Stage::CopyFiles, [&](const std::string &text) {
                const double fraction = percentOf(text);
                if (fraction >= 0.0)
                    progress = fraction;
                setProgress(Stage::CopyFiles, progress, fraction >= 0.0 ? "copying" : text.substr(0, 72));
            });
        }
        if (m_cancel.load() || code != 0) {
            finishStage(Stage::CopyFiles, false, code == -1 ? "stopped" : "the extractor stopped early");
            if (!m_cancel.load())
                fail("Copying the game files failed; the log shows why.");
            m_running = false;
            return;
        }
        std::lock_guard lock(m_mutex);
        m_stages[static_cast<size_t>(Stage::CopyFiles)].progress = 1.0;
    }
    finishStage(Stage::CopyFiles, true, humanSize(disc.size) + " into " + (options.workspace / "jogo").string());

    // 4. Translate the game's code: the Python translator, module by module.
    beginStage(Stage::TranslateCode);
    {
        // How many modules the table lists, for the progress bar.
        int total = 0;
        {
            const std::string count = "cd " + q(sourceCopy) + " && awk 'NF && $1 !~ /^#/ {n++} END {print n+0}' " +
                                      q(sourceCopy / "tools/translator/modules.tsv");
            const int code = shell(count, Stage::TranslateCode);
            if (code == 0) {
                std::lock_guard lock(m_mutex);
                total = atoi(m_log.empty() ? "0" : m_log.back().c_str());
            }
        }
        int done = 0;
        // the stamp says this gen/ belongs to this payload's translator, so the game itself does
        // not translate again on first Play (prepare.cpp compares it, then compiles nothing)
        const std::string command = "cd " + q(sourceCopy) + " && TF_JOGO=" + q(options.workspace / "jogo") +
                                    " python3 tools/translator/build_modules.py && "
                                    "printf '%s|' \"$(cat tools/translator_version.txt 2>/dev/null)\" > gen/translated.stamp && "
                                    "printf '%s\\n' " +
                                    q(disc.gameId) + " >> gen/translated.stamp";
        int code = -1;
        {
            double progress = total > 0 ? 0.0 : -1.0;
            code = shell(command, Stage::TranslateCode, [&](const std::string &text) {
                if (text.compare(0, 6, "recomp") == 0) {
                    ++done;
                    if (total > 0)
                        progress = static_cast<double>(done) / total;
                    setProgress(Stage::TranslateCode, progress, std::to_string(done) + " of " + std::to_string(total) +
                                                                  (total > 0 ? " modules" : " modules so far"));
                }
            });
        }
        if (m_cancel.load() || code != 0) {
            finishStage(Stage::TranslateCode, false, code == -1 ? "stopped" : "the translator stopped early");
            if (!m_cancel.load())
                fail("Translating the game's code failed; the log shows why.");
            m_running = false;
            return;
        }
    }
    finishStage(Stage::TranslateCode, true, "generated C in " + (sourceCopy / "gen").string());

    // 5. Compile the runtime: the same build a manual `sh tools/build_linux.sh` runs, with
    //    the Options page's job count; the [n/N] lines it prints drive the progress bar, and
    //    it installs the game into <workspace>/source (TagForceRecomp, tcc/, include/).
    beginStage(Stage::CompileRuntime);
    {
        if (!std::filesystem::is_regular_file(sourceCopy / "tools/build_linux.sh")) {
            // a payload older than the Linux runtime: everything it built stays ready to use
            finishStage(Stage::CompileRuntime, false, "not in this payload",
                        "This payload predates the Linux runtime: the translated code and the game files are ready in " +
                            options.workspace.string() + ", and Play says what comes next.");
            m_gap = true;
            m_running = false;
            return;
        }
        int total = 0, done = 0;
        double progress = -1.0;
        const std::string command = "cd " + q(sourceCopy) + " && JOBS=" +
                                    std::to_string(options.jobs > 0 ? options.jobs : 2) + " sh tools/build_linux.sh";
        const int code = shell(command, Stage::CompileRuntime, [&](const std::string &text) {
            // "[12/57] runtime/core/kernel.cpp" is one compiled source of the current run
            if (text.size() > 3 && text[0] == '[') {
                const size_t slash = text.find('/'), close = text.find(']', 1);
                if (slash != std::string::npos && close != std::string::npos && slash < close) {
                    const int n = atoi(text.c_str() + 1), of = atoi(text.c_str() + slash + 1);
                    if (n > 0 && of > 0) {
                        ++done;
                        if (of > total)
                            total = of;
                        setProgress(Stage::CompileRuntime, std::min(1.0, double(done) / std::max(1, total)),
                                    std::to_string(done) + " of " + std::to_string(total) + " sources");
                        return;
                    }
                }
            }
            setProgress(Stage::CompileRuntime, progress, text.substr(0, 72));
        });
        if (m_cancel.load() || code != 0) {
            finishStage(Stage::CompileRuntime, false, code == -1 ? "stopped" : "the compiler stopped early");
            if (!m_cancel.load())
                fail("Compiling the runtime failed; the log shows why (the Tools page lists the packages it needs).");
            m_running = false;
            return;
        }
    }
    finishStage(Stage::CompileRuntime, true, "the game is in " + sourceCopy.string());

    std::lock_guard lock(m_mutex);
    m_succeeded = true;
    m_running = false;
}

} // namespace tf::launcher
