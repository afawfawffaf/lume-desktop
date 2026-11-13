#pragma once

#include "core/settings/file_store.h"
#include "core/ui/music_background_protocol.h"
#include "core/ui/music_page_protocol.h"
#include "core/ui/music_preferences_protocol.h"
#include "core/widget/widget_preferences.h"
#include "platform/windows/music/music_page_window.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace lume::shell {

struct MusicPageSessionOptions {
    // Explicit UTF-8 filename, normally <settings directory>/music-page.json.
    std::string path;
    bool verification = false;
    // Optional test adapter, which must outlive the session.
    settings::IFileStore* files = nullptr;
};

// Owns the desktop music card and its saved geometry. The card is a second view
// of the shell's playback, never a second player: the shell pushes the same
// snapshot it pushes to the widget, and the card's commands go into the existing
// music command queue. Closing is the window's own business; IsOpen() is the
// single source of truth for whether a second double-click should open it again.
class MusicPageSession final {
public:
    using CommandHandler = std::function<void(const ui::MusicPageCommand&)>;
    // Preferences the card changed from its right-click menu. They belong to the
    // shell's shared music-preferences session, not to this window.
    using PreferencesHandler = std::function<void(const ui::MusicPreferencesCommand&)>;
    // Background media the card chose from its menu; shared with the main window.
    using BackgroundHandler = std::function<void(const ui::MusicBackgroundCommand&)>;
    // The card asked to open the manager's music settings page.
    using SettingsHandler = std::function<void()>;
    // The page's script is live and can receive Web messages.
    using ReadyHandler = std::function<void()>;

    // UI-thread only; the parent owns the COM apartment and the message pump.
    MusicPageSession(HINSTANCE, MusicPageSessionOptions, CommandHandler, PreferencesHandler = {},
                     BackgroundHandler = {}, SettingsHandler = {}, ReadyHandler = {});
    ~MusicPageSession();
    MusicPageSession(const MusicPageSession&) = delete;
    MusicPageSession& operator=(const MusicPageSession&) = delete;

    // Opens the card. False reports a missing page or an unusable WebView2.
    bool Open();
    void Close();
    bool IsOpen() const;
    // True once the card's document is loaded; a false Post before this is lost.
    bool Ready() const;
    HWND Handle() const;

    // Dropped until the page has loaded; it asks for a snapshot itself.
    void Post(const std::wstring& json);

    // The settings page's view of the card: its saved size and whether it is up.
    // Also accepts setSize/reset for the same scope; unrelated messages return
    // nullopt so the shell's other parsers can see them.
    std::wstring State() const;
    std::optional<std::wstring> HandleMessage(const std::wstring& message);

    // Relays the imported lyric font manifest to the card's own WebView.
    void ConfigureMusicFonts(std::wstring directory,
        platform::windows::MusicPageWindow::MusicFontAssetLookup lookup);

    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lume::shell
