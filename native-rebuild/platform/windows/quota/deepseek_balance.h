#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lume::platform::windows::quota {

// One cached reading of a provider's balance. `ok` is false while the value is
// unknown: a failed fetch must never be shown as a zero balance.
struct DeepSeekBalance {
    bool ok = false;
    // True when the account still has credit for API calls.
    bool available = false;
    // The balance entries the API returned, already formatted for display.
    struct Entry {
        std::string currency;  // "CNY" or "USD"
        std::string total;     // total_balance, as the API reported it
        std::string granted;   // granted_balance
        std::string topped;    // topped_up_balance
    };
    std::vector<Entry> entries;
    // Why the last fetch failed, when it did. Fixed tokens only, never a body.
    std::string error;  // "", "auth", "network", "parse", "key"
};

// Bounded HTTP GET for the balance endpoint. Runs on a worker thread; the
// callback receives the outcome once. `cancelled` is polled while the request is
// in flight so shutdown does not wait for a timeout.
using Cancelled = std::function<bool()>;
using BalanceReply = std::function<void(DeepSeekBalance)>;
void FetchDeepSeekBalance(std::string apiKey, std::chrono::milliseconds timeout,
                          const Cancelled& cancelled, BalanceReply reply);

// Parses a balance response body. Exposed for tests: the wire format is the
// only thing this module trusts, so it is checked without a network call.
DeepSeekBalance ParseDeepSeekBalance(std::string_view body, unsigned status);

}  // namespace lume::platform::windows::quota
