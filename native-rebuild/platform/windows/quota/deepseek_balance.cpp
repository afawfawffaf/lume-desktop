#include "platform/windows/quota/deepseek_balance.h"

#include "vendor/nlohmann/json.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>

namespace lume::platform::windows::quota {
namespace {
using Json = nlohmann::json;

struct InternetHandle {
    HINTERNET value = nullptr;
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET h) : value(h) {}
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

// The endpoint is fixed: an arbitrary URL from the page would make this a
// request reflector, and the key only ever goes to its own issuer.
constexpr wchar_t kHost[] = L"api.deepseek.com";
constexpr wchar_t kPath[] = L"/user/balance";
constexpr std::size_t kMaxBody = 64 * 1024;
// An API key is a short opaque token; anything else is a paste accident.
constexpr std::size_t kMaxKey = 512;

bool Printable(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte >= 0x21 && byte <= 0x7e;
    });
}

std::string Trimmed(std::string_view value) {
    auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(begin, end - begin + 1));
}
}  // namespace

DeepSeekBalance ParseDeepSeekBalance(std::string_view body, unsigned status) {
    DeepSeekBalance result;
    // The API answers 401/403 for a rejected key and 402 for an unfunded one;
    // only a 200 carries a balance worth showing.
    if (status == 401 || status == 403) { result.error = "auth"; return result; }
    if (status != 200) { result.error = "network"; return result; }
    const auto parsed = Json::parse(body.begin(), body.end(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) { result.error = "parse"; return result; }
    const auto available = parsed.find("is_available");
    if (available == parsed.end() || !available->is_boolean()) { result.error = "parse"; return result; }
    const auto infos = parsed.find("balance_infos");
    if (infos == parsed.end() || !infos->is_array()) { result.error = "parse"; return result; }
    for (const auto& item : *infos) {
        if (!item.is_object()) continue;
        DeepSeekBalance::Entry entry;
        const auto read = [&](const char* key) {
            const auto it = item.find(key);
            return it != item.end() && it->is_string() ? it->get<std::string>() : std::string{};
        };
        entry.currency = read("currency");
        entry.total = read("total_balance");
        entry.granted = read("granted_balance");
        entry.topped = read("topped_up_balance");
        // A recognised currency is what makes the row drawable; skip the rest
        // rather than print a number with no unit.
        if (entry.currency != "CNY" && entry.currency != "USD") continue;
        if (entry.total.empty()) continue;
        // The API reports amounts as decimal strings; anything else is a body
        // this build does not understand.
        if (entry.total.size() > 32 || entry.total.find_first_not_of("0123456789.") != std::string::npos)
            continue;
        result.entries.push_back(std::move(entry));
    }
    if (result.entries.empty()) { result.error = "parse"; return result; }
    result.available = available->get<bool>();
    result.ok = true;
    return result;
}

void FetchDeepSeekBalance(std::string apiKey, std::chrono::milliseconds timeout,
                          const Cancelled& cancelled, BalanceReply reply) {
    DeepSeekBalance result;
    const auto key = Trimmed(apiKey);
    if (!Printable(key) || key.size() > kMaxKey) {
        result.error = "key";
        if (reply) reply(std::move(result));
        return;
    }
    const auto giveUp = [&](const char* error) {
        result.error = error;
        if (reply) reply(std::move(result));
    };
    InternetHandle session(WinHttpOpen(L"LumeQuota/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value) { giveUp("network"); return; }
    const auto tick = static_cast<int>(std::clamp<long long>(timeout.count(), 1000, 60000));
    WinHttpSetTimeouts(session.value, tick, tick, tick, tick);
    InternetHandle connection(WinHttpConnect(session.value, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) { giveUp("network"); return; }
    InternetHandle request(WinHttpOpenRequest(connection.value, L"GET", kPath, nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE));
    if (!request.value) { giveUp("network"); return; }
    const std::wstring authorization = L"Authorization: Bearer " + std::wstring(key.begin(), key.end());
    if (!WinHttpSendRequest(request.value, authorization.c_str(), static_cast<DWORD>(authorization.size()),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        giveUp("network");
        return;
    }
    if (cancelled && cancelled()) { giveUp("network"); return; }
    if (!WinHttpReceiveResponse(request.value, nullptr)) { giveUp("network"); return; }
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        giveUp("network");
        return;
    }
    std::string body;
    for (;;) {
        if (cancelled && cancelled()) { giveUp("network"); return; }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.value, &available)) { giveUp("network"); return; }
        if (!available) break;
        if (body.size() + available > kMaxBody) { giveUp("parse"); return; }
        const auto offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.value, body.data() + offset, available, &read)) { giveUp("network"); return; }
        body.resize(offset + read);
        if (!read) break;
    }
    if (reply) reply(ParseDeepSeekBalance(body, status));
}

}  // namespace lume::platform::windows::quota
