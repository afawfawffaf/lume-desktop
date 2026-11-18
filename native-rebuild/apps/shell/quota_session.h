#pragma once

#include "platform/windows/quota/deepseek_balance.h"
#include "platform/windows/quota/deepseek_key_store.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace lume::shell {

struct QuotaSessionOptions {
    // Encrypted key file, normally <settings directory>/deepseek-key.bin.
    std::string keyPath;
    // How long a reading stays fresh before another fetch is allowed.
    std::chrono::milliseconds refresh{15 * 60 * 1000};
    std::chrono::milliseconds timeout{8000};
    // Runs on the worker when a fetch finishes; post a main-thread wake only.
    std::function<void()> wake;
    bool verification = false;
};

// Fetches the DeepSeek balance on a worker thread and keeps the last reading.
// It never blocks the UI thread and never sends the key anywhere but DeepSeek:
// the host asks for a refresh, this module decides whether one is due.
class QuotaSession final {
public:
    explicit QuotaSession(QuotaSessionOptions);
    ~QuotaSession();
    QuotaSession(const QuotaSession&) = delete;
    QuotaSession& operator=(const QuotaSession&) = delete;

    // Saves (or clears, when empty) the API key and fetches at once. Returns
    // false when the key was rejected by the store, never for a network failure.
    bool SetKey(std::string key);
    bool HasKey() const;
    void ClearKey();

    // Asks for a fetch if one is due. Cheap: safe to call on every tick.
    void Refresh(bool force = false);
    // True while a worker is in flight, so the host keeps pumping messages.
    bool Running() const;
    // Takes the reading a finished worker produced; empty when none is waiting.
    std::optional<platform::windows::quota::DeepSeekBalance> Drain();
    // The last reading, for the page drawn between fetches.
    const platform::windows::quota::DeepSeekBalance& Reading() const;
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lume::shell
