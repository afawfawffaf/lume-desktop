#include "platform/windows/clipboard/clipboard_history.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <sstream>

namespace lume::platform::windows::clipboard {
namespace {

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// UTF-8 in, UTF-16 out. A snippet that is not valid UTF-8 is dropped rather
// than drawn as replacement glyphs.
std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const auto needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), wide.data(), needed))
        return {};
    return wide;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const auto needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string narrow(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), needed,
                        nullptr, nullptr);
    return narrow;
}

// The clipboard says whether this content may be kept in a history at all. A
// password manager sets it to false, which is the only signal we need to stay
// out of the way.
bool AllowedInHistory() {
    const auto marker = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (!marker) return true;
    HANDLE handle = GetClipboardData(marker);
    if (!handle) return true;
    const auto* bytes = static_cast<const std::byte*>(GlobalLock(handle));
    if (!bytes) return true;
    DWORD value = 1;
    // The value is a DWORD; tolerate a shorter buffer rather than reading past it.
    const auto size = GlobalSize(handle);
    if (size >= sizeof(DWORD)) std::memcpy(&value, bytes, sizeof(DWORD));
    GlobalUnlock(handle);
    return value != 0;
}
}  // namespace

std::optional<ClipEntry> Read() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return std::nullopt;
    if (!OpenClipboard(nullptr)) return std::nullopt;
    struct Close {
        ~Close() { CloseClipboard(); }
    } close;

    if (!AllowedInHistory()) return std::nullopt;
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) return std::nullopt;
    const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!text) return std::nullopt;
    struct Unlock {
        HANDLE value;
        ~Unlock() { GlobalUnlock(value); }
    } unlock{handle};

    // The size is the allocation, not the string: the terminator decides the end.
    const auto bytes = GlobalSize(handle);
    if (bytes < sizeof(wchar_t)) return std::nullopt;
    std::size_t length = 0;
    while (length * sizeof(wchar_t) < bytes && text[length]) ++length;
    if (!length) return std::nullopt;

    ClipEntry entry;
    entry.text = Narrow(std::wstring(text, length));
    entry.at = NowMs();
    if (entry.text.empty()) return std::nullopt;
    return entry;
}

History::History(CaptureLimits limits) : limits_(limits) {
    if (!limits_.unpinned) limits_.unpinned = 30;
    if (!limits_.maximum) limits_.maximum = 200;
}

std::optional<ClipEntry> History::Add(std::string text) {
    if (text.empty() || text.size() > limits_.maxBytes) return std::nullopt;
    std::lock_guard lock(mutex_);
    // The same text copied twice in a row is one row, not two.
    if (!rows_.empty() && rows_.front().text == text) return std::nullopt;
    ClipEntry entry;
    entry.id = next_++;
    entry.text = std::move(text);
    entry.at = NowMs();
    rows_.push_front(entry);
    Trim();
    return entry;
}

void History::Trim() {
    // Pinned rows never age out, so the count to keep out of `unpinned` excludes
    // them; the hard `maximum` still bounds the whole list.
    std::size_t unpinned = 0;
    for (auto it = rows_.begin(); it != rows_.end();) {
        if (it->pinned) {
            ++it;
            continue;
        }
        if (++unpinned > limits_.unpinned) it = rows_.erase(it);
        else ++it;
    }
    while (rows_.size() > limits_.maximum) rows_.pop_back();
}

std::vector<ClipEntry> History::Rows() const {
    std::lock_guard lock(mutex_);
    return {rows_.begin(), rows_.end()};
}

std::size_t History::Size() const {
    std::lock_guard lock(mutex_);
    return rows_.size();
}

bool History::TogglePin(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    for (auto& row : rows_) {
        if (row.id != id) continue;
        row.pinned = !row.pinned;
        Trim();
        return true;
    }
    return false;
}

bool History::Remove(std::uint64_t id) {
    std::lock_guard lock(mutex_);
    const auto before = rows_.size();
    rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [&](const ClipEntry& row) {
        return row.id == id;
    }), rows_.end());
    return rows_.size() != before;
}

void History::Clear() {
    std::lock_guard lock(mutex_);
    // Pinned rows are the user's own choices; clearing history keeps them.
    rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [](const ClipEntry& row) {
        return !row.pinned;
    }), rows_.end());
}

namespace {
constexpr const char* kHeader = "lume-clipboard-1";
}

bool History::Save(const std::filesystem::path& path) const {
    std::vector<ClipEntry> rows;
    {
        std::lock_guard lock(mutex_);
        rows.assign(rows_.begin(), rows_.end());
    }
    // Written through a temporary and renamed so a crash cannot leave a file
    // that no longer parses.
    auto temporary = path;
    temporary += L".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << kHeader << '\n';
    for (const auto& row : rows) {
        // One line per entry: the timestamp and pin flag, then the text with
        // newlines escaped so the record stays on one line.
        out << row.at << '\t' << (row.pinned ? 1 : 0) << '\t';
        for (const char ch : row.text) {
            if (ch == '\n') out << "\\n";
            else if (ch == '\r') out << "\\r";
            else if (ch == '\\') out << "\\\\";
            else out << ch;
        }
        out << '\n';
    }
    out.close();
    if (!out) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

bool History::Load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line) || line != kHeader) return false;
    std::deque<ClipEntry> loaded;
    std::uint64_t next = 1;
    while (std::getline(in, line)) {
        const auto first = line.find('\t');
        if (first == std::string::npos) continue;
        const auto second = line.find('\t', first + 1);
        if (second == std::string::npos) continue;
        ClipEntry entry;
        try {
            entry.at = std::stoll(line.substr(0, first));
        } catch (...) {
            continue;
        }
        entry.pinned = line.substr(first + 1, second - first - 1) == "1";
        const auto encoded = line.substr(second + 1);
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] == '\\' && i + 1 < encoded.size()) {
                const auto nextChar = encoded[++i];
                entry.text.push_back(nextChar == 'n' ? '\n' : nextChar == 'r' ? '\r' : nextChar);
            } else {
                entry.text.push_back(encoded[i]);
            }
        }
        if (entry.text.empty() || entry.text.size() > limits_.maxBytes) continue;
        entry.id = next++;
        loaded.push_back(std::move(entry));
    }
    if (loaded.empty() && next == 1) return false;
    {
        std::lock_guard lock(mutex_);
        rows_ = std::move(loaded);
        next_ = next;
    }
    return true;
}

bool CopyToClipboard(const std::string& text) {
    const auto wide = Widen(text);
    if (wide.empty()) return false;
    if (!OpenClipboard(nullptr)) return false;
    struct Close {
        ~Close() { CloseClipboard(); }
    } close;
    if (!EmptyClipboard()) return false;
    const auto bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle) return false;
    if (void* target = GlobalLock(handle)) {
        std::memcpy(target, wide.c_str(), bytes);
        GlobalUnlock(handle);
    } else {
        GlobalFree(handle);
        return false;
    }
    // The clipboard owns the block on success; SetClipboardData takes it.
    if (!SetClipboardData(CF_UNICODETEXT, handle)) {
        GlobalFree(handle);
        return false;
    }
    return true;
}

std::wstring ToLine(const std::string& text, std::size_t maxChars) {
    // The island draws one line: a copied paragraph would otherwise run off the
    // capsule. Newlines and runs of spaces collapse so the line starts with
    // something readable instead of indentation.
    const auto wide = Widen(text);
    if (wide.empty()) return {};
    std::wstring line;
    line.reserve(std::min(wide.size(), maxChars) + 1);
    bool pendingSpace = false;  // a gap was seen but no character followed it yet
    for (const wchar_t ch : wide) {
        // Any whitespace becomes a single space between words, and never a
        // leading or trailing one.
        if (ch == L'\r' || ch == L'\n' || ch == L'\t' || ch == L' ') {
            pendingSpace = !line.empty();
            continue;
        }
        if (pendingSpace) {
            line.push_back(L' ');
            pendingSpace = false;
        }
        line.push_back(ch);
        if (line.size() >= maxChars) {
            line.push_back(L'\u2026');
            break;
        }
    }
    return line;
}

}  // namespace lume::platform::windows::clipboard
