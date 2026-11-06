// launcher_pipeline.h - the build, stage by stage, on a worker thread.
#pragma once

#include "launcher.h"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace tf::launcher {

struct PipelineOptions {
    std::filesystem::path source;    // payload/source (release) or the checkout
    std::filesystem::path workspace; // where the build runs and the game lives
    std::string disc;                // the chosen ISO
    int jobs = 1;
};

// Runs the stages in order, keeping the log and each stage's state for the
// window (or for stdout, with --build). cancel() stops the current process.
class Pipeline {
public:
    ~Pipeline();

    void start(const PipelineOptions &options); // no-op while running
    void cancel();
    bool running() const { return m_running.load(); }
    bool started() const { return m_started.load(); }
    bool succeeded() const { return m_succeeded; }
    bool stoppedAtGap() const { return m_gap.load(); } // ended at "not on Linux yet"
    std::string error() const;
    std::array<StageState, kStageCount> stages() const;
    std::vector<std::string> log(size_t limit) const;
    uint64_t logVersion() const { return m_logVersion; }

private:
    void run(PipelineOptions options);

    mutable std::mutex m_mutex;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_gap{false};
    bool m_succeeded = false;
    std::string m_error;
    std::array<StageState, kStageCount> m_stages{};
    std::deque<std::string> m_log;
    uint64_t m_logVersion = 0;
    std::atomic<pid_t> m_child{-1}; // the running stage's process group
};

// Seconds on a clock that never jumps (CLOCK_MONOTONIC).
double nowSeconds();

} // namespace tf::launcher
