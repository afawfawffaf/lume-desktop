#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lume::shell {

// The quota page's API key is free text, so it cannot travel through the fixed
// page-command enum the other settings use. This is a small dedicated message,
// parsed the same defensive way as the other free-text commands.
struct QuotaCommand {
    // "set" saves a key and fetches at once; "clear" forgets it; "refresh"
    // fetches again; "snapshot" only reports whether a key is stored.
    std::string action;
    std::string key;
};

// Accepts only the exact versioned shape and a bounded, printable key.
std::optional<QuotaCommand> ParseQuotaCommand(std::wstring_view message) noexcept;

// The WorkBuddy settings' only writes: connect a build (opening a browser),
// disconnect one account, or ask for a refresh. There is no slot number: the two
// builds are separate logins, and within a build the accounts are identified by
// the uid the API assigned, so how many are connected is not fixed.
struct WorkBuddyCommand {
    // "connect", "disconnect", "refresh", "snapshot".
    std::string action;
    std::string build;  // "desktop" or "ai"
    // The account to remove, for "disconnect". Never a credential.
    std::string id;
};

std::optional<WorkBuddyCommand> ParseWorkBuddyCommand(std::wstring_view message) noexcept;

// Reports whether a key is stored and whether a fetch is running. Never the key.
std::wstring EncodeQuotaState(bool hasKey, bool running, bool ok, bool available,
                              std::string_view error, std::string_view amount,
                              std::string_view currency);

// One WorkBuddy account as the settings page lists it. Never carries a token.
struct WorkBuddySlotState {
    std::string build;  // "desktop" or "ai"
    // The uid the API assigned; empty while a connection is still pending.
    std::string id;
    bool connected = false;
    std::string label;
    std::string status;
    double remaining = 0;
    std::string plan;
};

// Every account of both builds plus the two per-build page flags.
std::wstring EncodeWorkBuddyState(const std::vector<WorkBuddySlotState>& slots,
                                  bool desktopAvailable, bool aiAvailable);

}  // namespace lume::shell
