#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace lume::platform::windows {
// One observable step of a LX playlist sync. Progress events arrive while the
// child runs; exactly one Finished event closes the run.
struct LxSyncEvent {
    bool finished = false;
    bool failed = false;
    std::string stage;
    std::int64_t done = 0;
    std::int64_t total = 0;
    std::int64_t ok = 0;
    std::int64_t fail = 0;
    std::int64_t skip = 0;
    std::int64_t converted = 0;
    std::string error;
};

// Runs the LX export helper as a hidden child process and reports its
// line-oriented progress. The helper is the only thing that touches LX data;
// this class never invents a path or reads the playlist itself.
class LxSyncRunner final {
public:
    explicit LxSyncRunner(std::function<void()> wake);
    ~LxSyncRunner();
    LxSyncRunner(const LxSyncRunner&) = delete;
    LxSyncRunner& operator=(const LxSyncRunner&) = delete;

    // Returns false when the interpreter, the script or the child process is
    // unavailable; the caller reports the failure and leaves the library alone.
    bool Start(const std::wstring& script, const std::wstring& outputDirectory);
    bool Running() const;
    std::deque<LxSyncEvent> Drain();
    void Stop();

private:
    void ReadLoop();
    void ReadLines(bool& finished);
    void Push(LxSyncEvent event);

    std::function<void()> wake_;
    std::thread reader_;
    mutable std::mutex mutex_;
    std::deque<LxSyncEvent> events_;
    void* process_ = nullptr;
    void* pipe_ = nullptr;
    bool running_ = false;
    bool finished_ = false;
    bool stopping_ = false;
};
} // namespace lume::platform::windows
