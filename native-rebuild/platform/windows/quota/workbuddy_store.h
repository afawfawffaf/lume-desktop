#pragma once

#include "platform/windows/quota/workbuddy_balance.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lume::platform::windows::quota {

// One connected WorkBuddy account: the credentials plus where it came from.
struct WorkBuddySession {
    std::string accessToken;
    std::string refreshToken;
    std::string uid;
    std::string domain;
    std::string label;  // the account's display name, if known
};

// Every connected account, keyed by its uid. The count is not fixed: an account
// that is connected stays, and removing one leaves the rest untouched. The file
// on disk is always DPAPI-encrypted.
class WorkBuddyStore final {
public:
    // `path` is the encrypted blob; the parent directory must exist.
    explicit WorkBuddyStore(std::filesystem::path path);
    ~WorkBuddyStore();
    WorkBuddyStore(const WorkBuddyStore&) = delete;
    WorkBuddyStore& operator=(const WorkBuddyStore&) = delete;

    // Hard ceiling, only to bound the file and the page: a person with more than
    // 16 signed-in accounts is not a case worth designing for.
    static constexpr int MaxAccounts = 16;

    // Every stored account of one build, in the order they were first saved.
    std::vector<WorkBuddySession> Load(WorkBuddyKind) const;
    // Stores or replaces one account, identified by its uid.
    bool Save(WorkBuddyKind, const WorkBuddySession&) const;
    // Removes one account by uid. Removing an absent uid is not an error.
    void Clear(WorkBuddyKind, std::string_view uid) const;
    // True when at least one account of that build is stored.
    bool Any(WorkBuddyKind) const;

private:
    std::string ReadRaw() const;
    bool WriteRaw(std::string_view) const;
    std::filesystem::path path_;
};

}  // namespace lume::platform::windows::quota
