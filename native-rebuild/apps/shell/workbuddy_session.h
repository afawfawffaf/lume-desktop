#pragma once

#include "platform/windows/quota/workbuddy_balance.h"
#include "platform/windows/quota/workbuddy_store.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lume::shell {

// One WorkBuddy account as the settings page and the island see it. The count is
// not fixed: an account appears here once it is connected and stays until it is
// removed.
struct WorkBuddyAccount {
    platform::windows::quota::WorkBuddyKind kind =
        platform::windows::quota::WorkBuddyKind::Desktop;
    // Stable identity: the uid the API assigned. Empty while a slot is only a
    // pending connection.
    std::string id;
    bool connected = false;
    std::string label;
    // "idle", "waiting" (the user must finish in the browser), "fetching",
    // "ready", "expired", "error", "none".
    std::string status = "none";
    platform::windows::quota::WorkBuddyCredits credits;
};

struct WorkBuddySessionOptions {
    // Encrypted store for every account, normally <settings>/workbuddy-accounts.bin.
    std::string storePath;
    std::chrono::milliseconds refresh{15 * 60 * 1000};
    std::chrono::milliseconds timeout{10000};
    std::chrono::milliseconds loginWindow{10 * 60 * 1000};
    std::function<void()> wake;
    bool verification = false;
};

// Owns every connected WorkBuddy account: the browser handshake, the encrypted
// store and the per-account fetch. It never blocks the UI thread and never sends
// a token to an origin other than the one that issued it.
class WorkBuddySession final {
public:
    explicit WorkBuddySession(WorkBuddySessionOptions);
    ~WorkBuddySession();
    WorkBuddySession(const WorkBuddySession&) = delete;
    WorkBuddySession& operator=(const WorkBuddySession&) = delete;

    // Starts a login for one build. On success `authUrl` is the page to open;
    // the worker then polls until the user finishes or the window closes. Any
    // number of accounts may be added, one connection at a time.
    std::optional<std::string> Begin(platform::windows::quota::WorkBuddyKind);
    // Removes one account by its id.
    void Disconnect(platform::windows::quota::WorkBuddyKind, std::string_view id);
    // Asks every connected account for a refresh if one is due. Cheap.
    void Refresh(bool force = false);
    // Every known account: connected ones and any connection still in progress.
    std::vector<WorkBuddyAccount> Accounts() const;
    // Replaces any reading a worker finished; empty when none is waiting.
    std::optional<std::vector<WorkBuddyAccount>> Drain();
    bool Running() const;
    // True when at least one account of that build is connected.
    bool AnyConnected(platform::windows::quota::WorkBuddyKind) const;
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lume::shell
