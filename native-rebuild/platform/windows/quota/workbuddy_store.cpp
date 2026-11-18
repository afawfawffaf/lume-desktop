#include "platform/windows/quota/workbuddy_store.h"

#include "vendor/nlohmann/json.hpp"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <vector>

namespace lume::platform::windows::quota {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaxBlob = 256 * 1024;
constexpr std::size_t kMaxField = 8192;

// The blob is bound to its own path, so a copy of the file cannot be decrypted
// somewhere else. The version tag lets the format change later without guessing.
std::string Entropy(const std::filesystem::path& path) {
    auto lower = std::filesystem::absolute(path).lexically_normal().wstring();
    CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    std::string value = "Lume.Quota.WorkBuddy.v1|";
    value.append(reinterpret_cast<const char*>(lower.data()), lower.size() * sizeof(wchar_t));
    return value;
}

bool Protect(std::string_view input, std::string_view entropy, bool decrypt, std::string& output) {
    DATA_BLOB source{static_cast<DWORD>(input.size()),
                     reinterpret_cast<BYTE*>(const_cast<char*>(input.data()))};
    DATA_BLOB extra{static_cast<DWORD>(entropy.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(entropy.data()))};
    DATA_BLOB blob{};
    const auto ok = decrypt
        ? CryptUnprotectData(&source, nullptr, &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &blob)
        : CryptProtectData(&source, L"Lume WorkBuddy credentials", &extra, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &blob);
    if (!ok) return false;
    output.assign(reinterpret_cast<const char*>(blob.pbData), blob.cbData);
    SecureZeroMemory(blob.pbData, blob.cbData);
    LocalFree(blob.pbData);
    return true;
}

const char* KindName(WorkBuddyKind kind) {
    return kind == WorkBuddyKind::DesktopAi ? "ai" : "desktop";
}

bool Printable(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte >= 0x20 && byte <= 0x7e;
    });
}

bool Valid(const WorkBuddySession& session) {
    // The uid identifies the account, so it is required along with the token.
    return !session.accessToken.empty() && session.accessToken.size() <= kMaxField && Printable(session.accessToken) &&
        !session.uid.empty() && session.uid.size() <= kMaxField && Printable(session.uid) &&
        session.refreshToken.size() <= kMaxField && Printable(session.refreshToken) &&
        session.domain.size() <= kMaxField && Printable(session.domain) &&
        session.label.size() <= kMaxField;
}

// A uid is a UUID in practice, but the key is opaque: it only has to be a
// JSON-safe string, so it is used verbatim.
Json ToJson(const WorkBuddySession& session) {
    return Json{{"accessToken", session.accessToken}, {"refreshToken", session.refreshToken},
                {"uid", session.uid}, {"domain", session.domain}, {"label", session.label}};
}

std::optional<WorkBuddySession> FromJson(const Json& entry) {
    if (!entry.is_object()) return std::nullopt;
    const auto read = [&](const char* key) {
        const auto it = entry.find(key);
        return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    WorkBuddySession session;
    session.accessToken = read("accessToken");
    session.refreshToken = read("refreshToken");
    session.uid = read("uid");
    session.domain = read("domain");
    session.label = read("label");
    if (!Valid(session)) return std::nullopt;
    return session;
}

// The subject of the access token's payload: the account id the API itself
// signs into the token, so it identifies the account even for a record saved
// before the uid was stored next to it. Base64url, decoded without a library.
std::string SubjectOf(std::string_view jwt) { return AccountIdOf(jwt); }

// A record written before accounts were keyed by uid stored one object per slot
// instead of a list. Both shapes are read so an upgrade does not lose the
// accounts already authorised: the caller cannot re-authorise on the user's
// behalf.
std::vector<Json> EntriesOf(const Json& build) {
    std::vector<Json> list;
    if (build.is_array()) {
        for (const auto& entry : build) list.push_back(entry);
    } else if (build.is_object()) {
        for (auto it = build.begin(); it != build.end(); ++it)
            if (it->is_object()) list.push_back(*it);
    }
    return list;
}
}  // namespace

WorkBuddyStore::WorkBuddyStore(std::filesystem::path path) : path_(std::move(path)) {}
WorkBuddyStore::~WorkBuddyStore() = default;

std::string WorkBuddyStore::ReadRaw() const {
    if (path_.empty()) return {};
    HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    struct Close { HANDLE value; ~Close() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } close{file};
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    // A reparse point could redirect the read to another file entirely.
    if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(kMaxBlob))
        return {};
    std::string blob(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file, blob.data(), static_cast<DWORD>(blob.size()), &read, nullptr) || read != blob.size())
        return {};
    std::string plain;
    if (!Protect(blob, Entropy(path_), true, plain)) return {};
    return plain;
}

bool WorkBuddyStore::WriteRaw(std::string_view plain) const {
    if (path_.empty() || plain.size() > kMaxBlob) return false;
    std::string blob;
    if (!Protect(plain, Entropy(path_), false, blob)) return false;
    // Written through a temporary and renamed, so a crash cannot leave a
    // half-written blob that no longer decrypts.
    auto temporary = path_;
    temporary += L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const auto ok = WriteFile(file, blob.data(), static_cast<DWORD>(blob.size()), &written, nullptr) &&
        written == blob.size();
    CloseHandle(file);
    SecureZeroMemory(blob.data(), blob.size());
    if (!ok || !MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

std::vector<WorkBuddySession> WorkBuddyStore::Load(WorkBuddyKind kind) const {
    std::vector<WorkBuddySession> list;
    const auto plain = ReadRaw();
    if (plain.empty()) return list;
    const auto json = Json::parse(plain, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return list;
    const auto build = json.find(KindName(kind));
    if (build == json.end()) return list;
    for (auto& entry : EntriesOf(*build)) {
        // An account saved before the uid was stored beside it only carries the
        // token, but the token names its own subject, so the account can still be
        // identified instead of dropped.
        if (entry.is_object()) {
            const auto uid = entry.find("uid");
            if ((uid == entry.end() || !uid->is_string() || uid->get<std::string>().empty()) &&
                entry.contains("accessToken") && entry.at("accessToken").is_string())
                entry["uid"] = SubjectOf(entry.at("accessToken").get<std::string>());
        }
        if (auto session = FromJson(entry)) list.push_back(std::move(*session));
        // The order is the insertion order, so the page keeps a stable layout
        // between launches instead of sorting accounts by an opaque uid.
        if (static_cast<int>(list.size()) >= MaxAccounts) break;
    }
    return list;
}

bool WorkBuddyStore::Save(WorkBuddyKind kind, const WorkBuddySession& session) const {
    if (!Valid(session)) return false;
    Json root = Json::object();
    const auto plain = ReadRaw();
    if (!plain.empty()) {
        const auto existing = Json::parse(plain, nullptr, false);
        if (!existing.is_discarded() && existing.is_object()) root = existing;
    }
    // Whatever shape the build was stored in, it is rewritten as the list form:
    // a save is also the point where an older record is carried forward.
    Json kept = Json::array();
    bool replaced = false;
    if (root.contains(KindName(kind))) {
        for (auto& entry : EntriesOf(root[KindName(kind)])) {
            if (entry.is_object()) {
                const auto uid = entry.find("uid");
                if ((uid == entry.end() || !uid->is_string() || uid->get<std::string>().empty()) &&
                    entry.contains("accessToken") && entry.at("accessToken").is_string())
                    entry["uid"] = SubjectOf(entry.at("accessToken").get<std::string>());
                // Replacing an existing account keeps its position; a new one is
                // appended, so the page's order stays stable across saves.
                if (entry.value("uid", std::string{}) == session.uid) {
                    kept.push_back(ToJson(session));
                    replaced = true;
                    continue;
                }
            }
            kept.push_back(entry);
        }
    }
    if (!replaced) {
        if (static_cast<int>(kept.size()) >= MaxAccounts) return false;
        kept.push_back(ToJson(session));
    }
    root[KindName(kind)] = std::move(kept);
    return WriteRaw(root.dump());
}

void WorkBuddyStore::Clear(WorkBuddyKind kind, std::string_view uid) const {
    const auto plain = ReadRaw();
    if (plain.empty()) return;
    auto root = Json::parse(plain, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return;
    const auto build = root.find(KindName(kind));
    if (build == root.end()) return;
    Json kept = Json::array();
    std::size_t total = 0;
    for (auto& entry : EntriesOf(*build)) {
        ++total;
        if (entry.is_object()) {
            const auto id = entry.find("uid");
            if ((id == entry.end() || !id->is_string() || id->get<std::string>().empty()) &&
                entry.contains("accessToken") && entry.at("accessToken").is_string())
                entry["uid"] = SubjectOf(entry.at("accessToken").get<std::string>());
            if (entry.value("uid", std::string{}) == uid) continue;
        }
        kept.push_back(std::move(entry));
    }
    if (kept.size() == total) return;  // nothing matched
    // An empty build leaves nothing behind, so the file shrinks back to what it
    // holds rather than keeping an empty list for every product.
    if (kept.empty()) root.erase(KindName(kind));
    else root[KindName(kind)] = std::move(kept);
    if (root.empty()) { DeleteFileW(path_.c_str()); return; }
    WriteRaw(root.dump());
}

bool WorkBuddyStore::Any(WorkBuddyKind kind) const { return !Load(kind).empty(); }

}  // namespace lume::platform::windows::quota
