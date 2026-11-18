#include "platform/windows/quota/workbuddy_balance.h"

#include "vendor/nlohmann/json.hpp"

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>

#include <algorithm>
#include <cmath>

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

// The two builds are separate services with separate accounts and separate
// sign-in pages, so each one is pinned to its own origin. The API shape is
// identical; only the host and the login page differ. A token is only ever sent
// back to the build that issued it.
const wchar_t* HostOfKind(WorkBuddyKind kind) {
    return kind == WorkBuddyKind::DesktopAi ? L"www.workbuddy.ai" : L"www.codebuddy.cn";
}
constexpr std::size_t kMaxBody = 256 * 1024;
constexpr std::size_t kMaxToken = 8192;

struct Response {
    unsigned status = 0;
    std::string body;
};

// One HTTPS request to the origin of one build. The caller supplies only the
// path, so a token can never be sent to an origin the API did not choose.
Response Request(WorkBuddyKind kind, std::wstring_view method, std::wstring_view path,
                 std::string_view body,
                 const std::vector<std::pair<std::wstring, std::wstring>>& headers,
                 std::chrono::milliseconds timeout) {
    Response result;
    InternetHandle session(WinHttpOpen(L"LumeQuota/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value) return result;
    const auto tick = static_cast<int>(std::clamp<long long>(timeout.count(), 1000, 60000));
    WinHttpSetTimeouts(session.value, tick, tick, tick, tick);
    InternetHandle connection(WinHttpConnect(session.value, HostOfKind(kind), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.value) return result;
    InternetHandle request(WinHttpOpenRequest(connection.value, std::wstring(method).c_str(),
                                              std::wstring(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.value) return result;
    std::wstring joined;
    for (const auto& [name, value] : headers) {
        joined += name + L": " + value + L"\r\n";
    }
    const auto payload = body.empty() ? DWORD{0} : static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request.value, joined.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : joined.c_str(),
                            static_cast<DWORD>(joined.size()),
                            payload ? const_cast<char*>(body.data()) : WINHTTP_NO_REQUEST_DATA, payload,
                            payload, 0))
        return result;
    if (!WinHttpReceiveResponse(request.value, nullptr)) return result;
    DWORD status = 0, size = sizeof(status), available = 0;
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        // A transport failure is reported as status 0, which every caller treats
        // as "network", the same as a refused connection.
        return result;
    }
    result.status = status;
    for (;;) {
        if (!WinHttpQueryDataAvailable(request.value, &available)) return result;
        if (!available) break;
        if (result.body.size() + available > kMaxBody) return result;
        const auto offset = result.body.size();
        result.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.value, result.body.data() + offset, available, &read)) return result;
        result.body.resize(offset + read);
        if (!read) break;
    }
    return result;
}

std::vector<std::pair<std::wstring, std::wstring>> BaseHeaders() {
    // The API keys its behaviour off these two flags: without them it expects
    // a client session rather than answering the plugin handshake.
    return {{L"Accept", L"application/json"}, {L"X-Client-Platform", L"web"}};
}

std::wstring Widen(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

// The login page prefix is plain ASCII, so the narrow direction is a copy: the
// auth URL that comes back is a narrow string and has to be compared as one.
std::string Narrow(const wchar_t* value) {
    std::string text;
    for (; *value; ++value) text.push_back(static_cast<char>(*value));
    return text;
}
std::wstring Wide(const std::wstring& value) { return value; }
std::wstring Wide(std::wstring&& value) { return std::move(value); }

bool Printable(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte >= 0x21 && byte <= 0x7e;
    });
}

// The origin one build's sign-in page lives on. The API returns the page with
// its own host, and only that host counts as a login we started.
const wchar_t* LoginPrefixOf(WorkBuddyKind kind) {
    return kind == WorkBuddyKind::DesktopAi ? L"https://www.workbuddy.ai/login"
                                            : L"https://www.codebuddy.cn/login?";
}

// The platform tag tells the service which product the handshake is for, so the
// page it hands back belongs to that product.
const wchar_t* StatePathOf(WorkBuddyKind kind) {
    return kind == WorkBuddyKind::DesktopAi ? L"/v2/plugin/auth/state?platform=workbuddy-ai"
                                            : L"/v2/plugin/auth/state?platform=lume";
}
const wchar_t* TokenPath() { return L"/v2/plugin/auth/token"; }
const wchar_t* RefreshPath() { return L"/v2/plugin/auth/token/refresh"; }

// Shared by the poll, the refresh and every later fetch: the caller's token,
// the account id the API wants echoed, and the domain it was issued for.
std::vector<std::pair<std::wstring, std::wstring>> AuthHeaders(std::string_view token,
                                                              std::string_view uid,
                                                              std::string_view domain) {
    auto headers = BaseHeaders();
    headers.push_back({L"Authorization", L"Bearer " + Widen(token)});
    if (!uid.empty()) headers.push_back({L"X-User-Id", Widen(uid)});
    if (!domain.empty()) headers.push_back({L"X-Domain", Widen(domain)});
    return headers;
}
}  // namespace

WorkBuddyLogin BeginWorkBuddyLogin(WorkBuddyKind kind, std::chrono::milliseconds timeout) {
    WorkBuddyLogin result;
    auto headers = BaseHeaders();
    headers.push_back({L"X-No-Authorization", L"true"});
    headers.push_back({L"X-No-User-Id", L"true"});
    headers.push_back({L"Content-Type", L"application/json"});
    const auto response = Request(kind, L"POST", StatePathOf(kind), "{}", headers, timeout);
    if (response.status != 200) { result.error = "network"; return result; }
    const auto json = Json::parse(response.body.begin(), response.body.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object() || json.value("code", -1) != 0) {
        result.error = "parse";
        return result;
    }
    const auto data = json.find("data");
    if (data == json.end() || !data->is_object()) { result.error = "parse"; return result; }
    const auto state = data->find("state");
    const auto url = data->find("authUrl");
    if (state == data->end() || !state->is_string() || url == data->end() || !url->is_string()) {
        result.error = "parse";
        return result;
    }
    result.state = state->get<std::string>();
    result.authUrl = url->get<std::string>();
    // The URL has to point at the same origin this module talks to, and carry
    // the state it just issued: anything else is not a login we started.
    if (result.state.empty() || result.state.size() > 128 || !Printable(result.state) ||
        result.authUrl.rfind(Narrow(LoginPrefixOf(kind)), 0) != 0 ||
        result.authUrl.find("state=" + result.state) == std::string::npos) {
        result.error = "parse";
        result.state.clear();
        result.authUrl.clear();
    }
    return result;
}

// The token's own payload carries the account id, which is what the AI build's
// handshake relies on: it answers with tokens but no separate uid field.
std::string AccountIdOf(std::string_view token) {
    const auto dot = token.find('.');
    if (dot == std::string_view::npos) return {};
    const auto end = token.find('.', dot + 1);
    if (end == std::string_view::npos) return {};
    auto payload = std::string(token.substr(dot + 1, end - dot - 1));
    for (auto& ch : payload) {
        if (ch == '-') ch = '+';
        else if (ch == '_') ch = '/';
    }
    while (payload.size() % 4) payload.push_back('=');
    DWORD needed = 0;
    if (CryptStringToBinaryA(payload.c_str(), static_cast<DWORD>(payload.size()), CRYPT_STRING_BASE64,
                             nullptr, &needed, nullptr, nullptr) != TRUE || needed == 0)
        return {};
    std::string decoded(needed, '\0');
    if (CryptStringToBinaryA(payload.c_str(), static_cast<DWORD>(payload.size()), CRYPT_STRING_BASE64,
                             reinterpret_cast<BYTE*>(decoded.data()), &needed, nullptr, nullptr) != TRUE)
        return {};
    decoded.resize(needed);
    const auto json = Json::parse(decoded.begin(), decoded.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object()) return {};
    const auto it = json.find("sub");
    if (it == json.end() || !it->is_string()) return {};
    auto subject = it->get<std::string>();
    if (subject.empty() || subject.size() > kMaxToken || !Printable(subject)) return {};
    return subject;
}

WorkBuddyPoll PollWorkBuddyLogin(WorkBuddyKind kind, std::string_view state,
                                 std::chrono::milliseconds timeout) {
    WorkBuddyPoll result;
    if (state.empty() || state.size() > 128 || !Printable(state)) {
        result.error = "parse";
        return result;
    }
    auto headers = BaseHeaders();
    headers.push_back({L"X-No-Authorization", L"true"});
    const auto response = Request(kind, L"GET", std::wstring(TokenPath()) + L"?state=" + Widen(state), {},
                                  headers, timeout);
    if (response.status == 401 || response.status == 403) { result.error = "expired"; return result; }
    if (response.status != 200) { result.error = "network"; return result; }
    const auto json = Json::parse(response.body.begin(), response.body.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object()) { result.error = "parse"; return result; }
    // 11217 is the API's "still waiting for the user" code.
    if (json.value("code", -1) == 11217) { result.pending = true; return result; }
    if (json.value("code", -1) != 0) { result.error = "expired"; return result; }
    const auto data = json.find("data");
    if (data == json.end() || !data->is_object()) { result.error = "parse"; return result; }
    const auto read = [&](const char* key) {
        const auto it = data->find(key);
        return it != data->end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    result.accessToken = read("accessToken");
    result.refreshToken = read("refreshToken");
    result.uid = read("uid");
    result.domain = read("domain");
    if (result.accessToken.empty() || result.accessToken.size() > kMaxToken || !Printable(result.accessToken)) {
        result.error = "parse";
        return result;
    }
    // The AI build answers with tokens but no uid field. Dropping the sign-in
    // over that would lose an account the user just authorised, so the id is
    // taken from the token the service just signed.
    if (result.uid.empty()) result.uid = AccountIdOf(result.accessToken);
    result.ok = true;
    return result;
}

WorkBuddyPoll RefreshWorkBuddyToken(WorkBuddyKind kind, std::string_view refreshToken,
                                    std::string_view uid, std::string_view domain,
                                    std::chrono::milliseconds timeout) {
    WorkBuddyPoll result;
    if (refreshToken.empty() || refreshToken.size() > kMaxToken || !Printable(refreshToken)) {
        result.error = "expired";
        return result;
    }
    auto headers = BaseHeaders();
    headers.push_back({L"X-Refresh-Token", Widen(refreshToken)});
    headers.push_back({L"X-Auth-Refresh-Source", L"plugin"});
    if (!domain.empty()) headers.push_back({L"X-Domain", Widen(domain)});
    headers.push_back({L"Content-Type", L"application/json"});
    const auto response = Request(kind, L"POST", RefreshPath(), "{}", headers, timeout);
    if (response.status == 401 || response.status == 403) { result.error = "expired"; return result; }
    if (response.status != 200) { result.error = "network"; return result; }
    const auto json = Json::parse(response.body.begin(), response.body.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object() || json.value("code", -1) != 0) {
        result.error = "expired";
        return result;
    }
    const auto data = json.find("data");
    if (data == json.end() || !data->is_object()) { result.error = "parse"; return result; }
    const auto read = [&](const char* key) {
        const auto it = data->find(key);
        return it != data->end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    result.accessToken = read("accessToken");
    result.refreshToken = read("refreshToken");
    result.domain = read("domain");
    result.uid = std::string(uid);
    if (result.accessToken.empty() || !Printable(result.accessToken)) {
        result.error = "expired";
        return result;
    }
    // A refresh may rotate the refresh token; an absent one keeps the old value.
    if (result.refreshToken.empty()) result.refreshToken = std::string(refreshToken);
    if (result.domain.empty()) result.domain = std::string(domain);
    result.ok = true;
    return result;
}

WorkBuddyCredits ParseWorkBuddyCredits(std::string_view body, unsigned status) {
    WorkBuddyCredits result;
    if (status == 401 || status == 403) { result.error = "expired"; return result; }
    if (status != 200) { result.error = "network"; return result; }
    const auto json = Json::parse(body.begin(), body.end(), nullptr, false);
    if (json.is_discarded() || !json.is_object() || json.value("code", -1) != 0) {
        result.error = "parse";
        return result;
    }
    const auto data = json.find("data");
    if (data == json.end() || !data->is_object()) { result.error = "parse"; return result; }
    // The plan name is optional: a free account may not have one.
    const auto plan = data->find("SubscriptionPackageName");
    if (plan != data->end() && plan->is_string()) result.plan = plan->get<std::string>();
    // A number arrives as JSON either as a number or as a decimal string.
    const auto amountOf = [](const Json& value, bool& ok) {
        ok = false;
        double number = 0;
        if (value.is_number()) number = value.get<double>();
        else if (value.is_string()) {
            try { number = std::stod(value.get<std::string>()); } catch (...) { return 0.0; }
        } else return 0.0;
        if (!std::isfinite(number) || number < 0) return 0.0;
        ok = true;
        return number;
    };
    // CreditGroups, when present, is the authoritative breakdown: it splits the
    // balance into plan, bonus, add-ons and gifts.
    bool any = false;
    if (const auto groups = data->find("CreditGroups"); groups != data->end() && groups->is_array()) {
        for (const auto& group : *groups) {
            if (!group.is_object()) continue;
            const auto kind = group.find("Kind");
            if (kind == group.end() || !kind->is_string()) continue;
            std::string label;
            const auto value = kind->get<std::string>();
            if (value == "plan_base") label = "plan";
            else if (value == "plan_bonus") label = "bonus";
            else if (value == "addon") label = "addon";
            else if (value == "benefit") label = "benefit";
            else continue;
            const auto capacity = group.find("CycleRemainCapacity");
            if (capacity == group.end()) continue;
            bool ok = false;
            const auto amount = amountOf(*capacity, ok);
            if (!ok) continue;
            result.parts.push_back({label, amount});
            result.remaining += amount;
            any = true;
        }
    }
    // Older responses only carry Packages; fall back to it so an account whose
    // API has not rolled out groups still shows a figure.
    if (!any) {
        if (const auto packages = data->find("Packages"); packages != data->end() && packages->is_array()) {
            for (const auto& item : *packages) {
                if (!item.is_object()) continue;
                const auto capacity = item.find("CycleRemainCapacity");
                if (capacity == item.end()) continue;
                bool ok = false;
                const auto amount = amountOf(*capacity, ok);
                if (!ok) continue;
                result.parts.push_back({"plan", amount});
                result.remaining += amount;
                any = true;
            }
        }
    }
    if (!any) { result.error = "parse"; return result; }
    if (!std::isfinite(result.remaining)) { result.error = "parse"; result.parts.clear(); return result; }
    result.ok = true;
    return result;
}

WorkBuddyCredits FetchWorkBuddyCredits(WorkBuddyKind kind, std::string_view accessToken,
                                       std::string_view uid, std::string_view domain,
                                       std::chrono::milliseconds timeout) {
    WorkBuddyCredits result;
    if (accessToken.empty() || accessToken.size() > kMaxToken || !Printable(accessToken) ||
        !Printable(uid)) {
        result.error = "auth";
        return result;
    }
    auto headers = AuthHeaders(accessToken, uid, domain);
    headers.push_back({L"Content-Type", L"application/json"});
    const auto response = Request(kind, L"POST", L"/billing/meter/get-user-resource-summary", "{}", headers, timeout);
    return ParseWorkBuddyCredits(response.body, response.status);
}

}  // namespace lume::platform::windows::quota
