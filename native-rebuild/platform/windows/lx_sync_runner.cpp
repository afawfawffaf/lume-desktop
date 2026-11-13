#include "platform/windows/lx_sync_runner.h"
#include "platform/windows/text_util.h"
#include "vendor/nlohmann/json.hpp"

#include <windows.h>

#include <string>
#include <vector>

namespace lume::platform::windows {
namespace {
constexpr const char* kProgressPrefix = "LVP ";
constexpr const char* kFinishedPrefix = "LVEND ";
constexpr std::size_t kMaxLineBytes = 8192;

HANDLE AsHandle(void* value) { return static_cast<HANDLE>(value); }

std::wstring Quote(const std::wstring& value) { return L"\"" + value + L"\""; }

// The helper is a plain script run by whatever Python the machine already has;
// Lume never installs or bundles an interpreter.
std::wstring FindPython() {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD length = SearchPathW(nullptr, L"python.exe", nullptr,
                                     static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
    if (length > 0 && length < std::size(buffer)) return buffer;
    return L"python.exe";
}

void CloseHandleIf(void*& value) {
    if (value) {
        CloseHandle(AsHandle(value));
        value = nullptr;
    }
}
} // namespace

LxSyncRunner::LxSyncRunner(std::function<void()> wake) : wake_(std::move(wake)) {}

LxSyncRunner::~LxSyncRunner() {
    Stop();
    if (reader_.joinable()) reader_.join();
}

bool LxSyncRunner::Running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

bool LxSyncRunner::Start(const std::wstring& script, const std::wstring& outputDirectory) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return false;
    }
    if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &attributes, 0)) return false;
    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }

    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                   OPEN_EXISTING, 0, nullptr);

    std::wstring command = Quote(FindPython()) + L" " + Quote(script) +
                           L" --out " + Quote(outputDirectory);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = nullInput == INVALID_HANDLE_VALUE ? nullptr : nullInput;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &info);
    CloseHandle(writePipe);
    if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    if (!created) {
        CloseHandle(readPipe);
        return false;
    }
    CloseHandle(info.hThread);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        process_ = info.hProcess;
        pipe_ = readPipe;
        running_ = true;
        finished_ = false;
        stopping_ = false;
        events_.clear();
    }
    // Assigning to a joinable std::thread calls std::terminate, and a completed
    // read loop leaves its thread joinable until someone collects it, so a
    // second sync has to retire the previous one first.
    if (reader_.joinable()) reader_.join();
    reader_ = std::thread([this] { ReadLoop(); });
    return true;
}

void LxSyncRunner::ReadLoop() {
    bool finished = false;
    try {
        ReadLines(finished);
    } catch (...) {
        // A helper that dies must still settle the run; the page is waiting for
        // a finished event and nothing else will produce one.
    }
    DWORD exitCode = 1;
    if (process_) {
        WaitForSingleObject(AsHandle(process_), 5000);
        GetExitCodeProcess(AsHandle(process_), &exitCode);
    }
    if (!finished) {
        LxSyncEvent event;
        event.finished = true;
        event.failed = true;
        event.error = exitCode == 0 ? "sync-incomplete" : "sync-failed";
        Push(std::move(event));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    CloseHandleIf(pipe_);
    CloseHandleIf(process_);
    if (wake_) wake_();
}

void LxSyncRunner::ReadLines(bool& finished) {
    std::string pending;
    char buffer[4096];
    for (;;) {
        DWORD read = 0;
        const bool ok = ReadFile(AsHandle(pipe_), buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) != FALSE;
        if (!ok || read == 0) break;
        pending.append(buffer, read);
        std::size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() > kMaxLineBytes) continue;
            auto trim = [](std::string& value, const char* prefix) {
                const std::size_t size = std::char_traits<char>::length(prefix);
                if (value.size() < size || value.compare(0, size, prefix) != 0) return false;
                value.erase(0, size);
                return true;
            };
            try {
                if (trim(line, kProgressPrefix)) {
                    const auto json = nlohmann::json::parse(line);
                    LxSyncEvent event;
                    event.stage = json.value("stage", std::string());
                    event.done = json.value("done", std::int64_t{0});
                    event.total = json.value("total", std::int64_t{0});
                    Push(std::move(event));
                } else if (trim(line, kFinishedPrefix)) {
                    const auto json = nlohmann::json::parse(line);
                    LxSyncEvent event;
                    event.finished = true;
                    event.stage = json.value("stage", std::string());
                    event.ok = json.value("ok", std::int64_t{0});
                    event.fail = json.value("fail", std::int64_t{0});
                    event.skip = json.value("skip", std::int64_t{0});
                    event.converted = json.value("converted", std::int64_t{0});
                    event.total = json.value("total", std::int64_t{0});
                    event.error = json.value("error", std::string());
                    event.failed = !event.error.empty();
                    Push(std::move(event));
                    finished = true;
                }
            } catch (...) {
                // A partial or unexpected line is not progress; the finished
                // event is what settles the run.
            }
        }
    }
}

void LxSyncRunner::Push(LxSyncEvent event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(event));
    }
    if (wake_) wake_();
}

std::deque<LxSyncEvent> LxSyncRunner::Drain() {
    std::deque<LxSyncEvent> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.swap(events_);
    return out;
}

void LxSyncRunner::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    HANDLE process = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        process = AsHandle(process_);
    }
    if (process && Running()) {
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 2000);
    }
    if (reader_.joinable()) reader_.join();
}
} // namespace lume::platform::windows
