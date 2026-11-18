#include "apps/shell/quota_session.h"

#include <windows.h>
#include <filesystem>

#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace lume::shell {
namespace quota = platform::windows::quota;

struct QuotaSession::Impl {
    explicit Impl(QuotaSessionOptions options)
        : options(std::move(options)), store(this->options.keyPath.empty() ?
              std::filesystem::path{} : std::filesystem::u8path(this->options.keyPath)) {}

    ~Impl() { Shutdown(); }

    // Reads the key once; an unreadable blob behaves like no key.
    bool LoadKey() {
        auto loaded = store.Load();
        if (!loaded) return false;
        std::lock_guard lock(mutex);
        key = std::move(*loaded);
        return !key.empty();
    }

    void Kick(bool force) {
        if (stopped) return;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(mutex);
            if (running) return;
            if (key.empty()) return;
            // A manual refresh skips the interval, but never while one is in
            // flight: the worker already answers with the newest reading.
            if (!force && hasReading && now - fetchedAt < options.refresh) return;
            if (options.verification) return;
            running = true;
            std::string workKey = key;
            // The thread is joined below before a new one starts, so at most one
            // worker exists and the key never crosses threads while in use.
            if (worker.joinable()) worker.join();
            worker = std::thread([this, workKey = std::move(workKey)]() mutable {
                quota::DeepSeekBalance result;
                try {
                    quota::FetchDeepSeekBalance(workKey, options.timeout, [this] {
                        std::lock_guard guard(mutex);
                        return stopped;
                    }, [&](quota::DeepSeekBalance value) { result = std::move(value); });
                } catch (...) {
                    result = {};
                    result.error = "network";
                }
                // The key is raw memory of the secret; clear it before it dies.
                SecureZeroMemory(workKey.data(), workKey.size());
                Finish(std::move(result));
            });
            return;
        }
    }

    void Finish(quota::DeepSeekBalance result) {
        {
            std::lock_guard lock(mutex);
            running = false;
            if (stopped) return;
            reading = result;
            hasReading = true;
            fetchedAt = std::chrono::steady_clock::now();
            pending = std::move(result);
        }
        if (options.wake) options.wake();
    }

    void Shutdown() {
        std::thread join;
        {
            std::lock_guard lock(mutex);
            if (stopped) return;
            stopped = true;
            join = std::move(worker);
        }
        // The worker polls `stopped` between socket calls, so the join is bounded
        // by one receive timeout rather than the whole request.
        if (join.joinable()) join.join();
        SecureZeroMemory(key.data(), key.size());
        key.clear();
    }

    QuotaSessionOptions options;
    quota::DeepSeekKeyStore store;
    mutable std::mutex mutex;
    std::string key;
    std::thread worker;
    quota::DeepSeekBalance reading;
    std::optional<quota::DeepSeekBalance> pending;
    std::chrono::steady_clock::time_point fetchedAt{};
    bool running = false, stopped = false, hasReading = false;
};

QuotaSession::QuotaSession(QuotaSessionOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {
    impl_->LoadKey();
}
QuotaSession::~QuotaSession() = default;

bool QuotaSession::SetKey(std::string key) {
    if (impl_->stopped) return false;
    const auto trimmed = std::string(key.begin(), key.end());
    if (trimmed.empty()) { ClearKey(); return true; }
    if (!impl_->store.Save(trimmed)) return false;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->key = key;
        // A new key invalidates the old reading: it belonged to another account.
        impl_->hasReading = false;
    }
    Refresh(true);
    return true;
}

bool QuotaSession::HasKey() const {
    std::lock_guard lock(impl_->mutex);
    return !impl_->key.empty();
}

void QuotaSession::ClearKey() {
    impl_->store.Clear();
    std::lock_guard lock(impl_->mutex);
    SecureZeroMemory(impl_->key.data(), impl_->key.size());
    impl_->key.clear();
    impl_->hasReading = false;
    impl_->reading = {};
}

void QuotaSession::Refresh(bool force) { impl_->Kick(force); }

bool QuotaSession::Running() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->running;
}

std::optional<quota::DeepSeekBalance> QuotaSession::Drain() {
    std::lock_guard lock(impl_->mutex);
    auto result = std::move(impl_->pending);
    impl_->pending.reset();
    return result;
}

const quota::DeepSeekBalance& QuotaSession::Reading() const { return impl_->reading; }

void QuotaSession::Shutdown() { impl_->Shutdown(); }

}  // namespace lume::shell
