#pragma once

#include <cstddef>
#include <deque>
#include <filesystem>
#include <optional>
#include <functional>
#include <mutex>
#include <cstdint>
#include <string>
#include <vector>

namespace lume::platform::windows::clipboard {

// One entry the shelf can offer again. `pinned` entries survive the rolling
// limit so a snippet the user keeps is never pushed out by newer copies.
struct ClipEntry {
    // Stable identity for removing or pinning the right row, assigned on capture.
    std::uint64_t id = 0;
    // UTF-8. Newlines are kept; the page decides how to flatten them.
    std::string text;
    // Unix milliseconds, used only for ordering and to show "how long ago".
    std::int64_t at = 0;
    bool pinned = false;
};

// The capture rules, in one place so the reader and the tests agree:
//  - only CF_UNICODETEXT is kept; an image or a file drop is not text history
//  - a password manager's marking (CanIncludeInClipboardHistory == false) is
//    never recorded, which is what keeps generated passwords out of the list
//  - identical consecutive copies collapse into one row instead of filling it
struct CaptureLimits {
    // Unpinned entries beyond this many are dropped oldest-first.
    std::size_t unpinned = 30;
    // A single entry larger than this is ignored: it is a log paste, not a
    // snippet, and it would dominate the file.
    std::size_t maxBytes = 32 * 1024;
    // Entries kept in total, pinned included, so the file stays bounded.
    std::size_t maximum = 200;
};

// Reads the clipboard once. Returns nothing when the current content is not
// recordable text, is marked sensitive, or repeats the newest entry.
// Must be called on a thread that may touch the clipboard (the UI thread) and
// never while another window holds it open.
std::optional<ClipEntry> Read();

// The list itself. Bounded, newest first, with pinning. Thread safe so the
// capture thread and the drawing thread can share it.
class History final {
public:
    explicit History(CaptureLimits = {});
    History(const History&) = delete;
    History& operator=(const History&) = delete;

    // Adds one entry, assigning it an id. Returns the stored entry, or nullopt
    // when the text is empty, too large, or the same as the newest unpinned row.
    std::optional<ClipEntry> Add(std::string text);
    // The rows to draw, newest first.
    std::vector<ClipEntry> Rows() const;
    std::size_t Size() const;
    // Pinned rows stay when the rolling limit trims.
    bool TogglePin(std::uint64_t id);
    bool Remove(std::uint64_t id);
    void Clear();

    // Persistence. The format is a small UTF-8 text file; a partial or corrupt
    // file yields an empty history rather than an exception.
    bool Save(const std::filesystem::path&) const;
    bool Load(const std::filesystem::path&);

private:
    void Trim();
    mutable std::mutex mutex_;
    CaptureLimits limits_;
    std::deque<ClipEntry> rows_;
    std::uint64_t next_ = 1;
};

// Puts `text` back on the clipboard, so tapping a row re-copies it. Returns
// false when another window holds the clipboard open. UI thread only.
bool CopyToClipboard(const std::string& text);

// Turns a multi-line snippet into the single line the island can draw, cutting
// at a visible width. Exposed for the page and its tests.
std::wstring ToLine(const std::string& text, std::size_t maxChars);

}  // namespace lume::platform::windows::clipboard
