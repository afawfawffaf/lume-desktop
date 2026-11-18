#include "platform/windows/quota/deepseek_key_store.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <vector>

namespace lume::platform::windows::quota {
namespace {
// A short opaque token; anything longer is a paste accident, and the blob is
// bounded so a corrupt file cannot exhaust memory.
constexpr std::size_t kMaxKey = 512;
constexpr std::size_t kMaxBlob = 8 * 1024;

std::string Entropy(const std::filesystem::path& path) {
    // Binding the blob to its own path means a copied file cannot be decrypted
    // in place elsewhere, which is the same rule the other stores follow.
    auto lower = std::filesystem::absolute(path).lexically_normal().wstring();
    CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    std::string value = "Lume.Quota.DeepSeek.v1|";
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
        : CryptProtectData(&source, L"Lume DeepSeek API key", &extra, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &blob);
    if (!ok) return false;
    output.assign(reinterpret_cast<const char*>(blob.pbData), blob.cbData);
    SecureZeroMemory(blob.pbData, blob.cbData);
    LocalFree(blob.pbData);
    return true;
}

// Only ASCII printable keys are accepted, so an accidental paste of prose or a
// control character is refused before it is ever sent.
bool Printable(std::string_view key) {
    return !key.empty() && std::all_of(key.begin(), key.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte >= 0x21 && byte <= 0x7e;
    });
}
}  // namespace

DeepSeekKeyStore::DeepSeekKeyStore(std::filesystem::path path) : path_(std::move(path)) {}
DeepSeekKeyStore::~DeepSeekKeyStore() = default;

bool DeepSeekKeyStore::Exists() const {
    if (path_.empty()) return false;
    std::error_code error;
    return std::filesystem::is_regular_file(path_, error);
}

std::optional<std::string> DeepSeekKeyStore::Load() const {
    if (!Exists()) return std::nullopt;
    HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    struct Close { HANDLE value; ~Close() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } close{file};
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    // A reparse point could redirect the read to another file entirely.
    if (!GetFileInformationByHandle(file, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(kMaxBlob))
        return std::nullopt;
    std::string blob(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file, blob.data(), static_cast<DWORD>(blob.size()), &read, nullptr) || read != blob.size())
        return std::nullopt;
    std::string key;
    if (!Protect(blob, Entropy(path_), true, key)) return std::nullopt;
    if (key.size() > kMaxKey || !Printable(key)) {
        SecureZeroMemory(key.data(), key.size());
        return std::nullopt;
    }
    return key;
}

bool DeepSeekKeyStore::Save(std::string key) const {
    if (path_.empty()) return false;
    if (key.empty()) { Clear(); return true; }
    if (key.size() > kMaxKey || !Printable(key)) return false;
    std::string blob;
    if (!Protect(key, Entropy(path_), false, blob)) return false;
    SecureZeroMemory(key.data(), key.size());
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

void DeepSeekKeyStore::Clear() const {
    if (path_.empty()) return;
    DeleteFileW(path_.c_str());
}

}  // namespace lume::platform::windows::quota
