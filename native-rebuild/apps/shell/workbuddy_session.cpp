#include "apps/shell/workbuddy_session.h"

#include <windows.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>

namespace lume::shell {
namespace quota = platform::windows::quota;
using quota::WorkBuddyKind;

namespace {
int BuildOf(WorkBuddyKind kind) { return kind == WorkBuddyKind::DesktopAi ? 1 : 0; }

// One account's live state. `generation` invalidates a worker whose account was
// removed or replaced while it was in flight.
struct Entry {
    WorkBuddyAccount account;
    quota::WorkBuddySession session;
    bool fetching = false;
    std::chrono::steady_clock::time_point fetchedAt{};
    std::uint64_t generation = 0;
    std::shared_ptr<std::thread> worker;
};

// A connection that has not produced an account yet. It is its own record so the
// page can show "waiting for the browser" without inventing a fake account.
struct Pending {
    WorkBuddyKind kind = WorkBuddyKind::Desktop;
    std::string status = "waiting";
    std::uint64_t generation = 0;
    std::shared_ptr<std::thread> worker;
};
} // namespace

struct WorkBuddySession::Impl {
    explicit Impl(WorkBuddySessionOptions options)
        : options(std::move(options)),
          store(this->options.storePath.empty() ? std::filesystem::path{}
                                                : std::filesystem::u8path(this->options.storePath)) {
        for (int build = 0; build < 2; ++build) {
            const auto kind = build == 1 ? WorkBuddyKind::DesktopAi : WorkBuddyKind::Desktop;
            for (auto& session : store.Load(kind)) {
                Entry entry;
                entry.account.kind = kind;
                entry.account.id = session.uid;
                entry.account.connected = true;
                entry.account.label = session.label;
                entry.account.status = "idle";
                entry.session = std::move(session);
                entries.push_back(std::move(entry));
            }
        }
    }

    ~Impl() { Shutdown(); }

    Entry* Find(WorkBuddyKind kind, std::string_view id) {
        for (auto& entry : entries)
            if (entry.account.kind == kind && entry.account.id == id) return &entry;
        return nullptr;
    }

    void Publish(WorkBuddyKind kind, std::string_view id, std::uint64_t entryGeneration,
                 quota::WorkBuddyCredits credits, std::string status) {
        {
            std::lock_guard lock(mutex);
            if (stopped) return;
            auto* entry = Find(kind, id);
            if (!entry || entry->generation != entryGeneration) return;
            entry->fetching = false;
            entry->account.status = std::move(status);
            if (credits.ok) {
                entry->account.credits = credits;
                entry->account.status = "ready";
            } else if (!credits.error.empty()) {
                entry->account.credits = {};
                entry->account.status = credits.error;
            }
            entry->fetchedAt = std::chrono::steady_clock::now();
            changed = true;
        }
        if (options.wake) options.wake();
    }

    void StartFetch(WorkBuddyKind kind, std::string_view id, bool force) {
        if (stopped) return;
        Entry* entry = nullptr;
        {
            std::lock_guard lock(mutex);
            entry = Find(kind, id);
            if (!entry || !entry->account.connected || entry->session.accessToken.empty()) return;
            if (entry->fetching) return;
            const auto now = std::chrono::steady_clock::now();
            // A manual refresh skips the interval, but never while a fetch runs.
            if (!force && entry->account.status == "ready" && now - entry->fetchedAt < options.refresh)
                return;
            if (options.verification) return;
            entry->fetching = true;
            entry->account.status = "fetching";
        }
        // Copy what the worker needs while the lock is held, then run unlocked.
        std::string token, refresh, uid, domain;
        std::uint64_t entryGeneration = 0;
        {
            std::lock_guard lock(mutex);
            auto* current = Find(kind, id);
            if (!current) return;
            token = current->session.accessToken;
            refresh = current->session.refreshToken;
            uid = current->session.uid;
            domain = current->session.domain;
            entryGeneration = current->generation;
            // A previous worker for this account must finish before a new one
            // starts, so at most one request per account is ever in flight.
            if (current->worker && current->worker->joinable()) current->worker->join();
            const auto storePath = options.storePath;
            const auto timeout = options.timeout;
            const std::string accountId(id);
            current->worker = std::make_shared<std::thread>(
                [this, kind, accountId, entryGeneration, token, refresh, uid, domain, storePath, timeout] {
                    // Same rule as the login worker: a throw out of a thread ends
                    // the process, so a failed read must become a failed status.
                    try {
                    auto credits = quota::FetchWorkBuddyCredits(kind, token, uid, domain, timeout);
                    if (!credits.ok && credits.error == "expired" && !refresh.empty()) {
                        // One refresh attempt: a rejected access token that still
                        // has a refresh token is recoverable without the user.
                        const auto renewed = quota::RefreshWorkBuddyToken(kind, refresh, uid, domain, timeout);
                        if (renewed.ok) {
                            quota::WorkBuddySession next;
                            next.accessToken = renewed.accessToken;
                            next.refreshToken = renewed.refreshToken;
                            next.domain = renewed.domain;
                            next.uid = uid;
                            next.label = renewLabel(kind, accountId);
                            quota::WorkBuddyStore persistent(storePath.empty() ? std::filesystem::path{}
                                                                               : std::filesystem::u8path(storePath));
                            if (persistent.Save(kind, next)) {
                                std::lock_guard lock(mutex);
                                if (!stopped) {
                                    if (auto* entry = Find(kind, accountId);
                                        entry && entry->generation == entryGeneration) {
                                        entry->session = next;
                                        if (!next.label.empty()) entry->account.label = next.label;
                                    }
                                }
                            }
                            credits = quota::FetchWorkBuddyCredits(kind, next.accessToken, next.uid,
                                                                   next.domain, timeout);
                        }
                    }
                    const auto status = credits.ok ? std::string("ready")
                        : (credits.error.empty() ? std::string("error") : credits.error);
                    Publish(kind, accountId, entryGeneration, std::move(credits), status);
                    } catch (...) {
                        // Report it on the row instead of ending the process; the
                        // account stays connected and the next refresh retries.
                        Publish(kind, accountId, entryGeneration, {},
                                "network");
                    }
                });
        }
    }

    // The label is not part of the refresh response, so the stored one is kept.
    // Called from a worker with the lock released, so it takes the lock itself:
    // walking `entries` unlocked races with a connect adding a row, and the
    // vector can be reallocated under the iteration.
    std::string renewLabel(WorkBuddyKind kind, std::string_view id) {
        std::lock_guard lock(mutex);
        if (auto* entry = Find(kind, id)) return entry->account.label;
        return {};
    }

    void StartLogin(WorkBuddyKind kind, std::string state) {
        Pending pending;
        pending.kind = kind;
        pending.status = "waiting";
        std::uint64_t pendingGeneration = 0;
        {
            std::lock_guard lock(mutex);
            pending.generation = ++generation;
            pendingGeneration = pending.generation;
            pendings.push_back(pending);
        }
        const auto pendingWorker = pendingGeneration;
        const auto timeout = options.timeout;
        const auto window = options.loginWindow;
        const auto storePath = options.storePath;
        auto worker = std::make_shared<std::thread>([this, kind, pendingWorker, state, timeout, window, storePath] {
            // A std::thread that lets an exception escape calls std::terminate,
            // which ends the process rather than reporting a failed handshake.
            // Everything this worker can throw (allocation, filesystem) has to
            // end as an ordinary failure instead.
            try {
            const auto deadline = std::chrono::steady_clock::now() + window;
            for (;;) {
                {
                    std::lock_guard lock(mutex);
                    if (stopped || pendingWorker != this->generation) return;
                    const bool alive = std::any_of(pendings.begin(), pendings.end(),
                        [&](const Pending& p) { return p.generation == pendingWorker; });
                    if (!alive) return;
                }
                const auto poll = quota::PollWorkBuddyLogin(kind, state, timeout);
                if (poll.ok) {
                    quota::WorkBuddySession session;
                    session.accessToken = poll.accessToken;
                    session.refreshToken = poll.refreshToken;
                    session.uid = poll.uid;
                    session.domain = poll.domain;
                    quota::WorkBuddyStore persistent(storePath.empty() ? std::filesystem::path{}
                                                                       : std::filesystem::u8path(storePath));
                    const bool saved = persistent.Save(kind, session);
                    {
                        std::lock_guard lock(mutex);
                        if (stopped || pendingWorker != this->generation) return;
                        pendings.erase(std::remove_if(pendings.begin(), pendings.end(),
                            [&](const Pending& p) { return p.generation == pendingWorker; }), pendings.end());
                        if (saved) {
                            Entry entry;
                            entry.account.kind = kind;
                            entry.account.id = session.uid;
                            entry.account.connected = true;
                            entry.account.label = session.label;
                            entry.account.status = "fetching";
                            entry.session = session;
                            entries.push_back(std::move(entry));
                        }
                        changed = true;
                    }
                    if (saved) {
                        StartFetch(kind, session.uid, true);
                    } else if (options.wake) {
                        options.wake();
                    }
                    return;
                }
                if (!poll.pending) {
                    std::lock_guard lock(mutex);
                    if (stopped || pendingWorker != this->generation) return;
                    pendings.erase(std::remove_if(pendings.begin(), pendings.end(),
                        [&](const Pending& p) { return p.generation == pendingWorker; }), pendings.end());
                    changed = true;
                    if (options.wake) options.wake();
                    return;
                }
                // The user is still in the browser: wait a little and poll again.
                for (int i = 0; i < 10; ++i) {
                    {
                        std::lock_guard lock(mutex);
                        if (stopped || pendingWorker != this->generation) return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    std::lock_guard lock(mutex);
                    if (stopped || pendingWorker != this->generation) return;
                    pendings.erase(std::remove_if(pendings.begin(), pendings.end(),
                        [&](const Pending& p) { return p.generation == pendingWorker; }), pendings.end());
                    changed = true;
                    if (options.wake) options.wake();
                    return;
                }
            }
            } catch (...) {
                // Drop the pending row so the page stops saying "waiting" and the
                // user can try again, rather than leaving a connection that will
                // never finish.
                std::lock_guard lock(mutex);
                pendings.erase(std::remove_if(pendings.begin(), pendings.end(),
                    [&](const Pending& p) { return p.generation == pendingWorker; }), pendings.end());
                changed = true;
                if (options.wake) options.wake();
            }
        });
        {
            std::lock_guard lock(mutex);
            for (auto& p : pendings)
                if (p.generation == pendingWorker) p.worker = worker;
        }
    }

    void Shutdown() {
        std::vector<std::shared_ptr<std::thread>> workers;
        {
            std::lock_guard lock(mutex);
            if (stopped) return;
            stopped = true;
            ++generation;
            for (auto& entry : entries) {
                if (entry.worker) workers.push_back(entry.worker);
                if (!entry.session.accessToken.empty())
                    SecureZeroMemory(entry.session.accessToken.data(), entry.session.accessToken.size());
                entry.session = {};
            }
            for (auto& pending : pendings)
                if (pending.worker) workers.push_back(pending.worker);
        }
        // The pendings are cleared only after every worker has stopped: a worker
        // may be between its `stopped` check and its walk over this list, and
        // clearing it first would hand the thread a destroyed container.
        for (auto& worker : workers)
            if (worker && worker->joinable()) worker->join();
        std::lock_guard lock(mutex);
        pendings.clear();
    }

    WorkBuddySessionOptions options;
    quota::WorkBuddyStore store;
    mutable std::mutex mutex;
    std::vector<Entry> entries;
    std::deque<Pending> pendings;
    std::uint64_t generation = 0;
    bool stopped = false, changed = false;
};

WorkBuddySession::WorkBuddySession(WorkBuddySessionOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
WorkBuddySession::~WorkBuddySession() = default;

std::optional<std::string> WorkBuddySession::Begin(quota::WorkBuddyKind kind) {
    if (impl_->stopped) return std::nullopt;
    const auto login = quota::BeginWorkBuddyLogin(kind, impl_->options.timeout);
    if (!login.error.empty() || login.authUrl.empty()) return std::nullopt;
    impl_->StartLogin(kind, login.state);
    return login.authUrl;
}

void WorkBuddySession::Disconnect(quota::WorkBuddyKind kind, std::string_view id) {
    {
        std::lock_guard lock(impl_->mutex);
        auto& list = impl_->entries;
        const auto before = list.size();
        list.erase(std::remove_if(list.begin(), list.end(), [&](Entry& entry) {
            if (entry.account.kind != kind || entry.account.id != id) return false;
            // Invalidate the worker before the entry goes away: it may still be
            // polling, and it must not publish into a removed row.
            ++entry.generation;
            return true;
        }), list.end());
        if (list.size() != before) impl_->changed = true;
    }
    impl_->store.Clear(kind, id);
    if (impl_->options.wake) impl_->options.wake();
}

void WorkBuddySession::Refresh(bool force) {
    if (impl_->stopped) return;
    std::vector<std::pair<quota::WorkBuddyKind, std::string>> work;
    {
        std::lock_guard lock(impl_->mutex);
        for (const auto& entry : impl_->entries)
            if (entry.account.connected) work.emplace_back(entry.account.kind, entry.account.id);
    }
    for (const auto& [kind, id] : work) impl_->StartFetch(kind, id, force);
}

std::vector<WorkBuddyAccount> WorkBuddySession::Accounts() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<WorkBuddyAccount> list;
    list.reserve(impl_->entries.size() + impl_->pendings.size());
    for (const auto& entry : impl_->entries) list.push_back(entry.account);
    // A connection in progress shows as its own row so the page can say what is
    // happening; it becomes a real account once the browser handshake returns.
    for (const auto& pending : impl_->pendings) {
        WorkBuddyAccount account;
        account.kind = pending.kind;
        account.connected = false;
        account.status = pending.status;
        list.push_back(std::move(account));
    }
    return list;
}

std::optional<std::vector<WorkBuddyAccount>> WorkBuddySession::Drain() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->changed) return std::nullopt;
    impl_->changed = false;
    std::vector<WorkBuddyAccount> list;
    list.reserve(impl_->entries.size() + impl_->pendings.size());
    for (const auto& entry : impl_->entries) list.push_back(entry.account);
    for (const auto& pending : impl_->pendings) {
        WorkBuddyAccount account;
        account.kind = pending.kind;
        account.connected = false;
        account.status = pending.status;
        list.push_back(std::move(account));
    }
    return list;
}

bool WorkBuddySession::Running() const {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->pendings.empty()) return true;
    for (const auto& entry : impl_->entries)
        if (entry.fetching) return true;
    return false;
}

bool WorkBuddySession::AnyConnected(quota::WorkBuddyKind kind) const {
    std::lock_guard lock(impl_->mutex);
    return std::any_of(impl_->entries.begin(), impl_->entries.end(),
        [&](const Entry& entry) { return entry.account.kind == kind && entry.account.connected; });
}

void WorkBuddySession::Shutdown() { impl_->Shutdown(); }

} // namespace lume::shell
