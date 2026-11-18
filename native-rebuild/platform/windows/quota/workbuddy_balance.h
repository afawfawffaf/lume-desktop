#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lume::platform::windows::quota {

// Which WorkBuddy build an account belongs to. The two builds are separate
// products with separate accounts; they share the same billing API.
enum class WorkBuddyKind { Desktop, DesktopAi };

// One WorkBuddy account's monthly credit reading. The API reports a set of
// credit groups; the widget shows their sum plus the per-group breakdown.
struct WorkBuddyCredits {
    bool ok = false;
    // Sum of every non-expired group, in credits.
    double remaining = 0;
    // Named parts of the balance: plan allowance, bonus, add-ons, gifts.
    struct Part {
        std::string label;  // already localised to a fixed token
        double amount = 0;
    };
    std::vector<Part> parts;
    // The subscription the account is on, when the API names one.
    std::string plan;
    // "" on success; otherwise a fixed token: "auth", "expired", "network",
    // "parse", "none", "cancelled".
    std::string error;
};

// The OAuth handshake state for one connection attempt: the caller opens
// `authUrl` in a browser and then polls until `Finish` reports success.
struct WorkBuddyLogin {
    std::string state;
    std::string authUrl;
    std::string error;  // "" when the request was created
};

// Requests a one-time login URL. Pure HTTP; no browser is opened here.
WorkBuddyLogin BeginWorkBuddyLogin(WorkBuddyKind, std::chrono::milliseconds timeout);

// The result of one poll of the login. `pending` means the user has not
// finished in the browser yet.
struct WorkBuddyPoll {
    bool pending = false;
    bool ok = false;
    // The issued credentials, when `ok`.
    std::string accessToken;
    std::string refreshToken;
    std::string uid;
    std::string domain;
    std::string error;
};

// Polls once. The caller repeats this until it stops reporting `pending`.
WorkBuddyPoll PollWorkBuddyLogin(WorkBuddyKind, std::string_view state,
                                 std::chrono::milliseconds timeout);

// Fetches the credit summary for one account.
WorkBuddyCredits FetchWorkBuddyCredits(WorkBuddyKind, std::string_view accessToken,
                                       std::string_view uid, std::string_view domain,
                                       std::chrono::milliseconds timeout);

// Exchanges a refresh token for a new access token. Used when a fetch reports
// `expired`, so the user does not have to connect again every few weeks.
WorkBuddyPoll RefreshWorkBuddyToken(WorkBuddyKind, std::string_view refreshToken,
                                    std::string_view uid, std::string_view domain,
                                    std::chrono::milliseconds timeout);

// Parses a credit summary body. Exposed for tests: the wire shape is the only
// thing this module trusts.
WorkBuddyCredits ParseWorkBuddyCredits(std::string_view body, unsigned status);

// The account id carried in a token's own payload (`sub`). The AI build's
// handshake answers without a separate uid field, so the id is read from the
// token it just issued rather than treating the sign-in as failed.
std::string AccountIdOf(std::string_view token);

}  // namespace lume::platform::windows::quota
