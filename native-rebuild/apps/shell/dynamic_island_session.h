#pragma once

#include "core/dynamic_island/island_model.h"
#include "core/settings/file_store.h"
#include "core/ui/page_bridge.h"
#include "core/widget/widget_preferences.h"
#include "platform/windows/dynamic_island/island_window.h"
#include "platform/windows/dynamic_island/island_shelf_page.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace lume::shell {
struct DynamicIslandSessionOptions {
    std::string preferencesPath;
    // UI-thread ports. The host validates media identity and queues playback work.
    std::function<void(const std::wstring&)> notify;
    std::function<void(dynamic_island::Action, const dynamic_island::Media&)> control;
    std::function<std::optional<dynamic_island::Media>()> externalMedia;
    std::function<void(dynamic_island::Action)> externalControl;
    std::function<void(bool playing)> externalSetPlaying;
    std::function<void()> openSettings;
    std::function<std::uint64_t()> monotonicMillis;
    // The calendar todo store shared with the calendar and today todo widgets.
    platform::windows::IslandTodoSource todos;
    // Current weather, read on demand from the weather component. The island
    // never fetches, locates or stores weather itself.
    std::function<dynamic_island::Weather()> weather;
    std::function<void()> openWeatherSettings;
    std::function<void(bool)> weatherActive;
    // True once a DeepSeek key is stored: the quota page only joins the wheel
    // rotation when it can actually show something.
    std::function<bool()> quotaAvailable;
};

class DynamicIslandSession final {
public:
    DynamicIslandSession(HINSTANCE, DynamicIslandSessionOptions, settings::IFileStore* = nullptr);
    ~DynamicIslandSession();
    bool Restore();
    void Open();
    std::optional<std::wstring> HandlePage(ui::PageCommand);
    std::wstring State() const;
    void Update(dynamic_island::Media);
    void SetEnglish(bool);
    void RefreshTodos();
    // Lets the taskbar's edge reach the island: while the pointer rests on the
    // taskbar's reveal strip the island steps off the desktop so Explorer answers it.
    void SetRevealOnEdge(bool);
    // Re-reads the weather component and repaints the weather page.
    void RefreshWeather();
    // Pushes a fetched balance to the quota page. The island never fetches.
    void SetQuota(platform::windows::quota::DeepSeekBalance);
    // Pushes fetched credits per build; the island never fetches.
    void SetWorkBuddy(std::vector<platform::windows::IslandWorkBuddyAccount> desktop,
                      std::vector<platform::windows::IslandWorkBuddyAccount> ai);
    // Which builds have at least one connected account, so their pages join the
    // wheel rotation only when they can show something.
    void SetWorkBuddyAvailable(bool desktop, bool ai);
    // The shelf page's rows; the host reads the clipboard and the dropped paths.
    void SetShelf(std::vector<platform::windows::IslandClipRow> clipboard, std::wstring clipboardEmpty,
                  std::vector<platform::windows::IslandFileRow> files, std::wstring filesEmpty);
    // Called with the tapped row and whether it came from the clipboard tab.
    void SetShelfAction(std::function<void(int, platform::windows::IslandShelfAction)>);
    void SetFilesDropped(std::function<void(std::vector<std::string>)>);
    void SetFileDragOut(std::function<void(int)>);
    void SetShellDragging(bool);
    void SetPinned(bool);
    bool ShowNotice(const std::string& taskId,const std::wstring& title,const std::wstring& time);
    const widget::Preferences& Snapshot() const;
    void Shutdown();
    bool Running() const;
    HWND Handle() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
