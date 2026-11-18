#pragma once
#include "core/dynamic_island/island_model.h"
#include "core/widget/widget_preferences.h"
#include "core/calendar/calendar_date.h"
#include "platform/windows/quota/deepseek_balance.h"
#include "platform/windows/dynamic_island/island_workbuddy_page.h"
#include "platform/windows/dynamic_island/island_shelf_page.h"
#include "platform/windows/widget_ui/widget_canvas.h"
#include <windows.h>
#include <functional>
#include <memory>
#include <optional>
#include <cstdint>
#include <string>
#include <vector>

namespace lume::platform::windows {
struct IslandPresentation {
    dynamic_island::Media media;
    std::wstring time, date;
    unsigned remaining = 0;
    bool focusing = false, finished = false, english = false;
    // Whether the quota page belongs in the page list at all: it is hidden until
    // the user has saved a key, so an unused feature takes no wheel stop.
    bool quotaAvailable = false;
    // The same rule per WorkBuddy build: a page appears only once its accounts
    // are connected.
    bool workbuddyAvailable = false, workbuddyAiAvailable = false;
};
enum class IslandSize { Small, Standard, Large };
// Pages of the expanded island; the mouse wheel moves between them.
// Reminder lists the reminders shown in this session, once there is one.
// Weather shows the weather component's current reading (it never fetches).
// Performance reuses the performance widget's view and samples only while open.
// Quota is one page: a tab strip at the top chooses between the providers whose
// readings the host has pushed (DeepSeek, WorkBuddy, WorkBuddy AI). It takes a
// single wheel stop rather than one per provider.
// Shelf is one page with two tabs: the recent clipboard copies and the files
// dropped on the island. Both live in one wheel stop.
enum class IslandPage { Music, Calendar, Todo, Reminder, Weather, Performance, Quota, Shelf };
struct IslandTodo {
    std::string id;
    std::wstring title, time;
    bool completed = false, projected = false;
    std::uint32_t color = 0;  // RGB of the todo's colour label; 0 for none.
    std::wstring notes;
};
// The shared calendar todo store, read on demand (no copy is kept by the window).
struct IslandTodoSource {
    std::function<std::vector<IslandTodo>(calendar::Date)> forDay;
    std::function<bool(calendar::Date)> hasTodos;
    std::function<void(const IslandTodo&)> toggle;
    std::function<void()> openCalendar;
    std::function<void(const std::string&)> openTask;
    // Shared todo editor (same form as the calendar); an empty id creates one on the day.
    std::function<void(const std::string&, calendar::Date)> edit;
    std::function<void(const IslandTodo&)> remove;
    std::function<bool()> writable;
    // Current state of one todo, for the reminder page; empty when deleted.
    std::function<std::optional<IslandTodo>(const std::string&)> find;
};
// None keeps the classic opaque black capsule.
enum class IslandMaterial { None, Liquid, Frosted };
// User-facing visibility and layering choices.
struct IslandDisplay {
    bool hideFullscreen = true;  // Step aside for fullscreen apps, games and presentations.
    bool topmost = true;         // Above windows; false keeps it in the desktop layer.
    bool hoverExpand = false;    // Expand after resting the pointer on the island.
    bool autoHide = true;        // Idle: tuck against the screen top, leaving a sliver to hover.
    bool messages = true;        // Show new QQ / WeChat / ... messages (their taskbar button flashing).
    IslandSize size = IslandSize::Standard;
    IslandMaterial material = IslandMaterial::None;
    // Horizontal position along the top edge, in DIP from the work area's left.
    // -1 keeps the capsule centred, which is the default for a fresh profile.
    // The island never leaves the top edge; only its horizontal place changes.
    float offset = -1;
};
inline bool operator==(const IslandDisplay& a, const IslandDisplay& b) {
    return a.hideFullscreen == b.hideFullscreen && a.topmost == b.topmost &&
           a.hoverExpand == b.hoverExpand && a.size == b.size && a.material == b.material &&
           a.autoHide == b.autoHide && a.messages == b.messages && a.offset == b.offset;
}
inline bool operator!=(const IslandDisplay& a, const IslandDisplay& b) { return !(a == b); }

// UI-thread window. Owns only drawing, input and short-lived animation timers.
// It never activates, hooks the mouse only while expanded (to collapse on an
// outside click), hides at once when a fullscreen application or game is in
// front on its monitor, and tucks against the screen top over maximized windows.
// Square cover from a Media::artwork data URL, as the island draws it.
std::shared_ptr<const WidgetBitmap> DecodeMediaArtwork(const std::string&);
class IslandWindow final {
public:
    using Act = std::function<void(dynamic_island::Action)>;
    // Reports a drag that settled on a new horizontal offset, so the host can
    // save it. Only fires on release, never during the drag.
    using Moving = std::function<void(float offset)>;
    IslandWindow(HINSTANCE, Act);
    // Installed after construction; may be replaced at any time.
    void SetMoving(Moving);
    ~IslandWindow();
    bool Start(const widget::Preferences&, const IslandDisplay& = {});
    void Stop();
    // Applies new display options; recreates the window when layering changes.
    bool SetDisplay(const IslandDisplay&);
    // Slides the capsule along the top edge without recreating the window.
    void SetOffset(float offset);
    void Update(IslandPresentation);
    // Weather page content; the host reads it from the weather component.
    void SetWeather(dynamic_island::Weather);
    // Quota page content; the host fetches it and pushes the reading here.
    void SetQuota(quota::DeepSeekBalance);
    // WorkBuddy page content, one list per build; the host fetches it.
    void SetWorkBuddy(std::vector<IslandWorkBuddyAccount> desktop, std::vector<IslandWorkBuddyAccount> ai);
    // The shelf page's rows, one list per tab. The host reads the clipboard and
    // the dropped paths; the window only draws and reports taps.
    void SetShelf(std::vector<IslandClipRow> clipboard, std::wstring clipboardEmpty,
                  std::vector<IslandFileRow> files, std::wstring filesEmpty);
    // Called with the tapped row and whether it came from the clipboard tab.
    void SetShelfAction(std::function<void(int, IslandShelfAction)>);
    // Called with the paths dropped on the island, as UTF-8. The host records
    // them; the window never copies or opens a file itself.
    void SetFilesDropped(std::function<void(std::vector<std::string>)>);
    // Called with the file chip that was dragged away, so the host can hand the
    // file to the shell. The window never reads the filesystem.
    void SetFileDragOut(std::function<void(int)>);
    // Marks that the shell owns a modal drag right now, so the island holds its
    // place and its frame until it ends.
    void SetShellDragging(bool);
    // Keeps the island where it is: no idle tuck, no stepping aside for a
    // maximized window. Set by the music card's pin.
    void SetPinned(bool);
    void SetTodoSource(IslandTodoSource);
    // Shows a todo reminder in the island for a few seconds; false when the
    // island cannot show it now (stopped, stepping aside for a fullscreen app,
    // or in the desktop layer where windows may cover it).
    bool ShowNotice(const std::string& taskId, const std::wstring& title, const std::wstring& time);
    // Repaints after the shared todo store changed.
    void RefreshTodos();
    IslandPage Page() const;
    void Expand(bool);
    // True while the pointer rests on the taskbar's reveal strip: the window steps
    // off the desktop so Explorer answers the edge again, and returns afterwards.
    bool SetRevealOnEdge(bool);
    HWND Handle() const;
    bool Expanded() const;
    // True while the island is on screen (not hidden for idle or fullscreen).
    bool Shown() const;
    // Copy the actual composed foreground for rendering verification.
    WidgetBitmap Frame() const;
    // Verification: completes running animations immediately.
    void Settle();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
