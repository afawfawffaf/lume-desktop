#include "apps/shell/music_page_session.h"

#include "core/widget/widget_preferences_store.h"
#include "platform/windows/local_settings_file_store.h"
#include "vendor/nlohmann/json.hpp"

#include <set>
#include <utility>

namespace lume::shell {
using platform::windows::MusicPageWindow;
using platform::windows::MusicPageWindowOptions;
namespace {

using Json = nlohmann::json;

// The settings page speaks its own strict family, the same shape the desktop
// widget uses, so one malformed message cannot reach the card.
struct CardMessage {
    bool setSize = false;
    int width = 0, height = 0;
    bool reset = false;
};

std::optional<CardMessage> ParseCard(const std::wstring& input) {
    if (input.empty() || input.size() > 1024 || input.find(L'\0') != std::wstring::npos) return {};
    try {
        struct Invalid {};
        std::set<std::string> keys;
        const auto json = Json::parse(input.begin(), input.end(),
            [&](int depth, Json::parse_event_t event, Json& value) {
                if (depth > 1 || event == Json::parse_event_t::array_start ||
                    (event == Json::parse_event_t::object_start && depth != 0)) throw Invalid{};
                if (event == Json::parse_event_t::key &&
                    (!keys.insert(value.get<std::string>()).second || keys.size() > 5)) throw Invalid{};
                return true;
            });
        if (!json.is_object() || !json.contains("version") || !json["version"].is_number_integer() ||
            json["version"] != 1 || !json.contains("scope") || json["scope"] != "music-page" ||
            !json.contains("action") || !json["action"].is_string())
            return {};
        const auto action = json["action"].get<std::string>();
        if (action == "snapshot" && json.size() == 3) return CardMessage{};
        if (action == "reset" && json.size() == 3) return CardMessage{false, 0, 0, true};
        if (action == "setSize" && json.size() == 5 && json.contains("width") && json.contains("height") &&
            json["width"].is_number_integer() && json["height"].is_number_integer()) {
            const auto width = json["width"].get<int>(), height = json["height"].get<int>();
            if (width <= 0 || height <= 0) return {};
            return CardMessage{true, width, height, false};
        }
    } catch (...) {}
    return {};
}
} // namespace

struct MusicPageSession::Impl {
    Impl(HINSTANCE instance, MusicPageSessionOptions options, CommandHandler commandHandler,
         PreferencesHandler preferencesHandler, BackgroundHandler backgroundHandler,
         SettingsHandler settingsHandler, ReadyHandler readyHandler)
        : instance(instance), options(std::move(options)), commandHandler(std::move(commandHandler)),
          preferencesHandler(std::move(preferencesHandler)), backgroundHandler(std::move(backgroundHandler)),
          settingsHandler(std::move(settingsHandler)), readyHandler(std::move(readyHandler)),
          store(this->options.files ? *this->options.files : localFiles, MusicPageWindow::Limits) {}

    ~Impl() { Shutdown(); }

    void Persist(const widget::Preferences& bounds) {
        if (stopped || options.path.empty() || options.path.find('\0') != std::string::npos) return;
        if (!widget::Valid(bounds, MusicPageWindow::Limits)) return;
        try {
            if (store.Save(options.path, bounds)) saved = bounds;
        } catch (...) {}
    }

    bool Open() {
        if (stopped) return false;
        if (window.IsOpen()) return true;
        // The saved rectangle is the starting point for a fresh window; the
        // window re-reads it itself, this copy is for State() before it opens.
        if (!options.path.empty() && options.path.find('\0') == std::string::npos) {
            try {
                const auto loaded = store.Load(options.path);
                if (loaded.state != widget::LoadState::Failed) saved = loaded.value;
            } catch (...) {}
        }
        return window.Open(
            instance,
            [this](const std::wstring& message) {
                if (stopped) return;
                // The settings page and the card share one preferences queue, so
                // the card's edits must reach the shell's music-preferences
                // session rather than the window's own private channel.
                if (const auto command = ui::ParseMusicPreferencesPageJson(message)) {
                    if (!preferencesHandler) return;
                    const auto callback = preferencesHandler;
                    try { callback(*command); } catch (...) {}
                    return;
                }
                // Background media is shared with the main window, so the card's
                // choice goes through the same session that owns the file.
                if (const auto command = ui::ParseMusicBackgroundPageJson(message)) {
                    if (!backgroundHandler) return;
                    const auto callback = backgroundHandler;
                    try { callback(*command); } catch (...) {}
                    return;
                }
                if (!commandHandler) return;
                // The card's own window gestures reach the shell here: "settings"
                // opens the manager's music page, which is where the same
                // controls live for anyone who prefers the full panel.
                if (message.find(L"music-page-window") != std::wstring::npos &&
                    message.find(L"\"settings\"") != std::wstring::npos) {
                    if (settingsHandler) {
                        const auto callback = settingsHandler;
                        try { callback(); } catch (...) {}
                    }
                    return;
                }
                const auto command = ui::ParseMusicPageJson(message);
                if (!command) return;
                const auto callback = commandHandler;
                try { callback(*command); } catch (...) {}
            },
            [this] {
                // The page asks for its first frame as soon as its script runs,
                // so the card paints immediately instead of waiting for a tick.
                if (stopped || !readyHandler) return;
                const auto callback = readyHandler;
                try { callback(); } catch (...) {}
            },
            {},
            [this](const widget::Preferences& bounds) { Persist(bounds); }, L"",
            MusicPageWindowOptions{options.path, options.verification,
                                   options.files ? options.files : &localFiles});
    }

    void Close() { window.Close(); }
    bool IsOpen() const { return window.IsOpen(); }

    void Shutdown() {
        if (stopped) return;
        stopped = true;
        commandHandler = {};
        preferencesHandler = {};
        backgroundHandler = {};
        settingsHandler = {};
        readyHandler = {};
        window.Close();
    }

    widget::Preferences Current() const {
        if (window.IsOpen()) {
            const auto live = window.Bounds();
            if (widget::Valid(live, MusicPageWindow::Limits)) return live;
        }
        return saved;
    }

    std::wstring State() const {
        const auto bounds = Current();
        const auto text = Json{
            {"version", 1}, {"type", "music-page-state"},
            {"open", window.IsOpen()}, {"stopped", stopped},
            {"width", bounds.width}, {"height", bounds.height},
            {"minWidth", MusicPageWindow::Limits.minWidth},
            {"minHeight", MusicPageWindow::Limits.minHeight},
            {"maxWidth", MusicPageWindow::Limits.maxWidth},
            {"maxHeight", MusicPageWindow::Limits.maxHeight},
        }.dump();
        // Only fixed ASCII fields and integers cross the boundary.
        return std::wstring(text.begin(), text.end());
    }

    HINSTANCE instance;
    MusicPageSessionOptions options;
    CommandHandler commandHandler;
    PreferencesHandler preferencesHandler;
    BackgroundHandler backgroundHandler;
    SettingsHandler settingsHandler;
    ReadyHandler readyHandler;
    platform::windows::LocalSettingsFileStore localFiles;
    widget::PreferencesStore store;
    MusicPageWindow window;
    widget::Preferences saved = widget::Defaults(MusicPageWindow::Limits);
    bool stopped = false;
};

MusicPageSession::MusicPageSession(HINSTANCE instance, MusicPageSessionOptions options,
                                   CommandHandler commandHandler, PreferencesHandler preferencesHandler,
                                   BackgroundHandler backgroundHandler, SettingsHandler settingsHandler,
                                   ReadyHandler readyHandler)
    : impl_(std::make_unique<Impl>(instance, std::move(options), std::move(commandHandler),
                                   std::move(preferencesHandler), std::move(backgroundHandler),
                                   std::move(settingsHandler), std::move(readyHandler))) {}
MusicPageSession::~MusicPageSession() = default;
bool MusicPageSession::Open() { return impl_->Open(); }
void MusicPageSession::Close() { impl_->Close(); }
bool MusicPageSession::IsOpen() const { return impl_->IsOpen(); }
bool MusicPageSession::Ready() const { return impl_->window.Ready(); }
HWND MusicPageSession::Handle() const { return impl_->window.Handle(); }
void MusicPageSession::Post(const std::wstring& json) { impl_->window.Post(json); }

void MusicPageSession::ConfigureMusicFonts(std::wstring directory, MusicPageWindow::MusicFontAssetLookup lookup) {
    impl_->window.ConfigureMusicFonts(std::move(directory), std::move(lookup));
}

std::wstring MusicPageSession::State() const { return impl_->State(); }

std::optional<std::wstring> MusicPageSession::HandleMessage(const std::wstring& message) {
    const auto parsed = ParseCard(message);
    if (!parsed) return {};
    if (impl_->stopped) return impl_->State();
    if (parsed->reset) {
        impl_->Persist(widget::Defaults(MusicPageWindow::Limits));
        return impl_->State();
    }
    if (parsed->setSize) {
        impl_->Persist(widget::Clamped(widget::Preferences{false, false, true, false, false, 0x2c2c2e,
                                                          impl_->saved.x, impl_->saved.y,
                                                          parsed->width, parsed->height},
                                       MusicPageWindow::Limits));
        return impl_->State();
    }
    return impl_->State();
}

void MusicPageSession::Shutdown() { impl_->Shutdown(); }

} // namespace lume::shell
