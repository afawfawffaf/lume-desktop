#pragma once
#include "core/widget/widget_preferences.h"
#include "core/settings/file_store.h"
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace lume::platform::windows {
// Options for the standalone music page. The window owns its saved geometry so
// the shell does not have to grow another store just for one rectangle.
struct MusicPageWindowOptions {
    std::string path;                 // music-page.json
    bool verification = false;
    settings::IFileStore *files = nullptr;
};

// The standalone music page: a resizable top-level window hosting
// ui/music-page.html, shown over the desktop rather than inside the manager
// window. Like the notes page it has no message loop of its own; the shell's UI
// thread pumps it.
class MusicPageWindow final {
public:
    using MessageHandler = std::function<void(const std::wstring &)>;
    // Shared with the shell so the geometry it saves is validated by the same
    // limits the window enforces.
    static constexpr widget::Limits Limits{900, 620, 480, 360, 1600, 1200, false};
    MusicPageWindow();
    ~MusicPageWindow();
    MusicPageWindow(const MusicPageWindow &) = delete;
    MusicPageWindow &operator=(const MusicPageWindow &) = delete;

    // UI-thread, non-reentrant manifest lookup: exact filename -> expected bytes.
    // Zero, missing callback or exception denies access. Never return a file path.
    using MusicFontAssetLookup = std::function<std::uint64_t(std::wstring_view)>;
    void ConfigureMusicFonts(std::wstring fontDirectory, MusicFontAssetLookup lookup = {});

    bool Open(HINSTANCE instance, MessageHandler onMessage, std::function<void()> ready = {},
              std::function<void()> closed = {},
              std::function<void(const widget::Preferences &)> boundsChanged = {},
              const std::wstring &profile = {}, MusicPageWindowOptions options = {});
    void Configure(const widget::Preferences &);
    widget::Preferences Bounds() const;
    void RequestClose();
    void Close();
    void Post(const std::wstring &json);
    bool IsOpen() const;
    // True once the document has loaded and PostWebMessageAsJson is accepted.
    // Callers must not treat a dropped Post as delivered.
    bool Ready() const;
    HWND Handle() const;

private:
    struct State;
    std::shared_ptr<State> state_;
    std::wstring fontDirectory_;
    MusicFontAssetLookup fontLookup_;
};
} // namespace lume::platform::windows
