#include "apps/shell/desktop_application.h"
#include "platform/windows/text_util.h"
#include "platform/windows/clipboard/clipboard_history.h"
#include "platform/windows/organizer/organizer_icons.h"
#include "platform/windows/organizer/organizer_icon_client.h"
#include "platform/windows/organizer/shell_drag.h"
#include "platform/windows/lume_window_ids.h"
#include "platform/windows/ui_stall_trace.h"
#include "apps/shell/calendar_session.h"
#include "platform/windows/music_desktop_controls.h"
#include "platform/windows/calendar/calendar_todo_editor.h"
#include "core/todo/todo_time.h"
#include "apps/shell/performance_session.h"
#include "apps/shell/spectrum_session.h"
#include "apps/shell/resource_snapshot.h"
#include "apps/shell/almanac_session.h"
#include "apps/shell/today_todo_session.h"
#include "apps/shell/weather_session.h"
#include "platform/windows/weather/weather_view.h"
#include "apps/shell/organizer_session.h"
#include "apps/shell/desktop_icons_session.h"
#include "apps/shell/taskbar_session.h"
#include "apps/shell/minimize_animation_session.h"
#include "apps/shell/music_preferences_session.h"
#include "apps/shell/notes_session.h"
#include "core/settings/json_settings_codec.h"
#include "core/settings/settings_store.h"
#include "apps/shell/diagnostic_operations.h"
#include "apps/shell/diagnostic_report.h"
#include "apps/shell/self_check.h"
#include "core/application_settings/application_settings.h"
#include "platform/windows/diagnostics/runtime_log.h"
#include "apps/shell/island_adapter.h"
#include "apps/shell/external_lyrics.h"
#include "core/ui/clock_page_state.h"
#include "core/ui/system_feature_page_commands.h"
#include "core/ui/music_library_protocol.h"
#include "platform/windows/clock_runtime.h"
#include "platform/windows/local_settings_file_store.h"
#include "platform/windows/music_login_diagnosis.h"
#include "platform/windows/music_score_source.h"
#include "platform/windows/lx_sync_runner.h"
#include "platform/windows/webview_page_window.h"
#include "platform/windows/reminder/local_reminder_runtime.h"
#include "platform/windows/shell_activation.h"
#include "platform/windows/music_system_fonts.h"
#include "platform/windows/widget_ui/desktop_level.h"
#include <objbase.h>
#include <filesystem>
#include <deque>
#include <set>
#include <map>
#include <mutex>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>
#include <thread>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <utility>
#include "core/ui/utf8_convert.h"

namespace lume::shell {
using namespace platform::windows;
namespace {
using lume::text::Utf8;
using lume::text::Wide;
TaskbarPaths TaskbarConfiguration(const DesktopApplicationOptions& options) {
    auto paths=options.taskbarPaths;
    if(options.verification)paths.allowSystemEffects=false;
    return paths;
}
MinimizeAnimationScope AnimationScope(const DesktopApplicationOptions& options) {
    if(!options.verification)return {};
    auto scope=options.verificationMinimizeScope;
    // An omitted verification fixture must never install desktop-wide hooks.
    if(!scope.threadId){scope.threadId=GetCurrentThreadId();scope.processId=GetCurrentProcessId();}
    return scope;
}
WallpaperSessionOptions WallpaperOptions(const DesktopApplicationOptions& options,bool balancedMode,
    std::function<void(std::string,std::string)> downloaded,
    std::function<void(std::string,std::string,std::function<void(std::string)>)> authorize) {
    auto wallpaper=options.wallpaperOptions;
    wallpaper.verification=options.verification;
    wallpaper.balancedMode=balancedMode;
    if(!wallpaper.downloadCompleted)wallpaper.downloadCompleted=std::move(downloaded);
    if(!options.verification)wallpaper.authorizeDownload=std::move(authorize);
    return wallpaper;
}
ApplicationSettingsOptions SettingsOptions(const DesktopApplicationOptions& options) {
    auto settings=options.applicationSettings;settings.verification=options.verification;return settings;
}
AccountSessionOptions AccountOptions(const DesktopApplicationOptions& options) {
    auto account=options.accountOptions;account.verification=options.verification;return account;
}
MusicSessionOptions MusicOptions(const DesktopApplicationOptions& options,
                                 std::function<void()> wake) {
    auto music = options.music;
    if (!music.wake) music.wake = std::move(wake);
    return music;
}
MusicProviderAccountOptions MusicAccountOptions(const DesktopApplicationOptions& options,
                                                std::function<void()> wake) {
    auto accounts = options.musicAccounts;
    accounts.verification = options.verification;
    accounts.wake = std::move(wake);
    return accounts;
}
MusicBackgroundSessionOptions MusicBackgroundOptions(const DesktopApplicationOptions& options,
                                                      std::function<void()> wake) {
    auto background = options.musicBackground;
    background.verification = options.verification;
    background.initiallyVisible = false;
    background.wake = std::move(wake);
    if(background.wallpaperLibraryRoot.empty())background.wallpaperLibraryRoot=options.wallpaperOptions.root;
    return background;
}
MusicFontAssetSessionOptions MusicFontOptions(const DesktopApplicationOptions& options,
                                             std::function<std::vector<std::string>()> selected) {
    auto fonts=options.musicFonts;
    fonts.verification=options.verification;
    if(!fonts.selectedFamilies)fonts.selectedFamilies=std::move(selected);
    return fonts;
}
UpdateNotificationOptions UpdateOptions(const DesktopApplicationOptions& options,
                                       UpdateNotificationSession::RequestCheck check,
                                       std::function<bool(const update_notification::Notice&)> show) {
    auto notifications=options.updateNotification;notifications.verification=options.verification;
    // Only an explicitly injected update transport may drive a verification schedule.
    if(options.verification&&options.applicationSettings.testPorts.updates&&!notifications.testPorts.requestCheck)
        notifications.testPorts.requestCheck=std::move(check);
    if(!notifications.showInApp)notifications.showInApp=std::move(show);
    return notifications;
}
}
struct DesktopApplication::State {
    // The calendar's todo form, opened from the island below the capsule. It
    // writes through the shared TodoSession, so every todo view refreshes.
    void OpenIslandTodoEditor(const std::string& id,calendar::Date day){
        if(exiting||!todos)return;
        if(islandEditor.IsOpen()){SetForegroundWindow(islandEditor.Handle());return;}
        const auto snapshot=todos->Read();
        if(!snapshot.writable)return;
        const auto& time=todos->Time();const auto now=WindowsLocalTime::NowMillis();
        todo::Command seed;seed.scope=snapshot.scope;std::optional<todo::TaskView> existing;
        if(!id.empty()){
            const auto found=std::find_if(snapshot.tasks.begin(),snapshot.tasks.end(),[&](const auto& t){return t.id==id&&!t.deletedAt;});
            if(found==snapshot.tasks.end())return;
            existing=*found;seed.id=found->id;seed.type=todo::CommandType::Update;
        } else {
            seed.patch.schedule=todo::DefaultSchedule(day,now,time);
            if(!seed.patch.schedule)return;
        }
        const HWND owner=dynamicIsland.Handle();
        const unsigned dpi=owner?GetDpiForWindow(owner):GetDpiForSystem();
        MONITORINFO monitor{sizeof(monitor)};
        if(!GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&monitor))return;
        const auto work=monitor.rcWork;
        const int width=std::min<int>(work.right-work.left,MulDiv(480,static_cast<int>(dpi),96));
        const int height=std::min<int>(work.bottom-work.top,MulDiv(560,static_cast<int>(dpi),96));
        // Below the island, but never past the bottom of a short work area.
        const int left=work.left+(work.right-work.left-width)/2,
                  top=std::max<int>(work.top,std::min<int>(work.top+MulDiv(64,static_cast<int>(dpi),96),work.bottom-height));
        calendar::Preferences appearance;appearance.material=calendar::Material::FrostedGlass;
        islandEditor.Open(instance,owner,RECT{left,top,left+width,top+height},dpi,appearance,time,seed,now,existing,
            [this](const todo::Command& command){
                todo::CommandResult result;result.error=todo::Error::ReadOnly;
                if(exiting||!todos)return result;
                return todos->Dispatch(command);
            },{},WindowsLocalTime::NowMillis);
    }
    // Music from another player, for its minimized window in the Dock.
    std::optional<NowPlaying> NowPlayingCover() {
        const auto media=systemMedia.Snapshot();
        if(!media||media->app.empty()||media->artwork.empty())return std::nullopt;
        if(media->artwork!=dockCoverKey){dockCoverKey=media->artwork;dockCover=platform::windows::DecodeMediaArtwork(dockCoverKey);}
        if(!dockCover)return std::nullopt;
        return NowPlaying{media->app,dockCover};
    }
    QuotaSessionOptions QuotaOptions() {
        QuotaSessionOptions value;
        // The key lives beside every other setting; the file itself is the
        // DPAPI blob, never plaintext.
        if(!options.musicPreferencesPath.empty()) {
            const auto directory=std::filesystem::u8path(options.musicPreferencesPath).parent_path();
            value.keyPath=(directory/L"deepseek-key.bin").u8string();
        }
        value.verification=options.verification;
        value.wake=[this]{Wake();};
        return value;
    }
    WorkBuddySessionOptions WorkBuddyOptions() {
        WorkBuddySessionOptions value;
        if(!options.musicPreferencesPath.empty()) {
            const auto directory=std::filesystem::u8path(options.musicPreferencesPath).parent_path();
            value.storePath=(directory/L"workbuddy-accounts.bin").u8string();
        }
        value.verification=options.verification;
        value.wake=[this]{Wake();};
        return value;
    }
    // Where the shelf keeps its history. Next to the other per-user settings so
    // a diagnostic report can find it, and so it survives a restart.
    std::filesystem::path ClipboardPath() const {
        if(options.musicPreferencesPath.empty()) return {};
        return std::filesystem::u8path(options.musicPreferencesPath).parent_path()/L"clipboard-history.txt";
    }
    DynamicIslandSessionOptions IslandOptions() {
        DynamicIslandSessionOptions value;
        value.preferencesPath=options.dynamicIslandPreferencesPath;
        value.notify=[this](const std::wstring& message){if(!exiting){Post(message);Wake();}};
        value.control=[this](dynamic_island::Action action,const dynamic_island::Media& media){
            const auto snapshot=music.Snapshot();
            if(exiting||!snapshot.current||snapshot.session!=media.session||snapshot.current->id!=media.id)return;
            ui::MusicPageCommand command;command.session=media.session;command.id=media.id;
            command.action=action==dynamic_island::Action::Previous?ui::MusicAction::Previous:
                action==dynamic_island::Action::Next?ui::MusicAction::Next:ui::MusicAction::Toggle;
            musicCommands.push_back(command);Wake();
        };
        if(!options.verification) {
            value.externalMedia=[this]{
                auto media=systemMedia.Snapshot();
                if(media)externalLyrics.Apply(*media);
                else externalLyrics.Clear();
                return media;
            };
            value.externalControl=[this](dynamic_island::Action action){systemMedia.Control(action);};
            value.externalSetPlaying=[this](bool playing){systemMedia.SetPlaying(playing);};
        }
        value.openSettings=[this]{Open(ManagerPage::DynamicIsland);};
        // Same store as the calendar and today todo widgets: reads go to it on
        // demand and toggles dispatch through it, so every view stays in sync.
        value.todos.forDay=[this](calendar::Date day){return exiting?std::vector<platform::windows::IslandTodo>{}:islandTodos.ForDay(day);};
        value.todos.find=[this](const std::string& id)->std::optional<platform::windows::IslandTodo>{if(exiting)return std::nullopt;return islandTodos.Find(id);};
        value.todos.hasTodos=[this](calendar::Date day){return !exiting&&islandTodos.HasTodos(day);};
        value.todos.toggle=[this](const platform::windows::IslandTodo& row){if(!exiting)islandTodos.Toggle(row);};
        value.todos.openCalendar=[this]{if(!exiting)Open(ManagerPage::Calendar);};
        value.todos.openTask=[this](const std::string& id){if(!exiting&&!calendar.OpenTodo(id))Open(ManagerPage::Calendar);};
        value.todos.writable=[this]{return !exiting&&todos&&todos->Read().writable;};
        value.todos.edit=[this](const std::string& id,calendar::Date day){OpenIslandTodoEditor(id,day);};
        value.todos.remove=[this](const platform::windows::IslandTodo& row){if(!exiting)islandTodos.Remove(row);};
        // Weather page: a bounded projection of the weather component's current
        // reading. Reads are on demand, so the island adds no fetching of its own.
        value.weather=[this]{return exiting?dynamic_island::Weather{}:IslandWeather(weather.Reading());};
        value.openWeatherSettings=[this]{if(!exiting)Open(ManagerPage::Weather);};
        value.weatherActive=[this](bool active){weather.SetSharedConsumer(active);};
        // The page appears in the wheel only once a key is saved, so an unused
        // feature does not take a stop away from the pages that work.
        value.quotaAvailable=[this]{return !exiting&&quota.HasKey();};
        return value;
    }
    State(HINSTANCE instance,DesktopApplicationOptions options):instance(instance),options(std::move(options)),
        diagnostics(diagnostics::FeatureEnabled ? this->options.diagnosticsPath : std::string{}),
        preferences(store.Load(this->options.clockPreferencesPath)),
        clockRuntime(instance,[this](clock::Action action){ClockAction(action);}),
        clock(preferences.clock,clockRuntime,[this](const settings::ClockSettings& value){
            auto next=preferences;next.clock=value;
            if(!store.Save(this->options.clockPreferencesPath,next))return false;
            preferences=std::move(next);return true;
        }),todos(std::make_shared<TodoSession>(this->options.calendarPreferencesPath)),
        calendar(instance,this->options.calendarPreferencesPath,
            [this](const std::wstring& message,bool attention){Post(message);if(remindersStarted)reminders.UpdateAppearance(Appearance());if(attention)Open(ManagerPage::Calendar);Wake();},
            [this]{Open(ManagerPage::Calendar);},todos),
        todayTodo(instance,this->options.todayTodoPreferencesPath,
            Notifier(),
            [this]{Open(ManagerPage::TodayTodo);},todos->Port()),
        performance(instance,this->options.performancePreferencesPath,
            Notifier(ManagerPage::Performance),
            [this]{Open(ManagerPage::Performance);}),
        spectrum(instance,this->options.spectrumPreferencesPath,
            Notifier(ManagerPage::Spectrum),
            [this]{Open(ManagerPage::Spectrum);}),
        almanac(instance,this->options.almanacPreferencesPath,
            Notifier(ManagerPage::Almanac),
            [this]{Open(ManagerPage::Almanac);}),
        weather(instance,this->options.weatherPreferencesPath,
            [this](const std::wstring& message,bool attention){Post(message);if(attention)Open(ManagerPage::Weather);dynamicIsland.RefreshWeather();Wake();},
            [this]{Open(ManagerPage::Weather);}),
        organizer(instance,this->options.organizerPreferencesPath,
            Notifier(ManagerPage::Organizer),
            [this]{Open(ManagerPage::Organizer);},nullptr,{},{},!this->options.verification),
        radial(instance,[this]{auto value=this->options.radial;value.verification=this->options.verification;return value;}(),
            Notifier(ManagerPage::Radial),
            [this](bool shortcut){radialShortcutSettings=shortcut;Open(ManagerPage::Radial);}),
        dock(instance,[this]{auto value=this->options.dock;value.verification=this->options.verification;
                if(!value.nowPlaying)value.nowPlaying=[this]{return NowPlayingCover();};return value;}(),
            Notifier(ManagerPage::Dock),
            [this]{Open(ManagerPage::Home);},[this]{Open(ManagerPage::Dock);},[this]{return visible;}),
        systemMedia(),
        dynamicIsland(instance, IslandOptions()),
        quota(QuotaOptions()),
        workbuddy(WorkBuddyOptions()),
        clipboardPath(ClipboardPath()),
        desktopIcons(instance,this->options.desktopIconsPreferencesPath,
            Notifier(ManagerPage::Icons),
            nullptr,this->options.verification&&this->options.verificationDesktopIcons?
                this->options.verificationDesktopIcons:FindDesktopIconsTargets),
        taskbar(instance,TaskbarConfiguration(this->options),
            [this](const std::wstring& message,bool attention){
                Post(message);
                if(attention)Open(ManagerPage::Taskbar);
                // The runtime can change the preference on its own (a restart, a
                // journal repair); the island follows whatever it settled on.
                dynamicIsland.SetRevealOnEdge(taskbar.Preferences().revealOnEdge &&
                    taskbar.Preferences().hideBehavior == taskbar::HideBehavior::AutoHide);
                Wake();
            },
            nullptr,this->options.verification?this->options.verificationTaskbar:TaskbarRuntime::PortFactory{}),
        minimize(instance,this->options.minimizePreferencesPath,
            Notifier(ManagerPage::MinimizeAnimation),
            nullptr,AnimationScope(this->options)),
        applicationSettings(instance,SettingsOptions(this->options),
            [this](const std::wstring& message){
                Post(message);
                if(!exiting){dynamicIsland.SetEnglish(applicationSettings.Preferences().language=="en");updateNotifications.SetLanguage(applicationSettings.Preferences().language=="en");updateNotifications.Wake();Wake();}
            },
            [this](bool balanced){return wallpaper.SetBalancedMode(balanced);}),
        wallpaper(instance,WallpaperOptions(this->options,applicationSettings.BalancedMode(),
            [this](std::string id,std::string source){if(!exiting)account.RecordWallpaperDownload(std::move(id),std::move(source));},
            [this](std::string id,std::string source,std::function<void(std::string)> completed){
                if(exiting){completed("WALLPAPER_CANCELLED");return;}
                account.AuthorizeWallpaperDownload(std::move(id),std::move(source),std::move(completed));}),
            Poster()),
        musicBackground(MusicBackgroundOptions(this->options,[this]{Wake();}),files),
        account(instance,AccountOptions(this->options),
            Poster()),
        music(PlaybackOptions()),
        musicAccounts(MusicAccountOptions(this->options,[this]{Wake();})),
        musicOnline(OnlineOptions()),
        musicLibrary(LibraryOptions()),
        musicDesktop(instance,DesktopMusicOptions(),
            [this](const ui::MusicPageCommand& command){
                const auto current=music.Snapshot();
                if(exiting||command.session!=current.session||!current.current||command.id!=current.current->id)return;
                musicCommands.push_back(command);Wake();
            },[this]{Open(ManagerPage::MusicDesktop);},
            // A double-click on the widget cover toggles the desktop card: the
            // same gesture opens it and puts it away again.
            [this]{ToggleMusicPage();}),
        musicPage(instance,MusicPageSessionOptions{this->options.musicPagePath,this->options.verification,&files},
            [this](const ui::MusicPageCommand& command){
                const auto current=music.Snapshot();
                if(exiting||command.session!=current.session||!current.current||command.id!=current.current->id)return;
                musicCommands.push_back(command);Wake();
            },
            // The card's menu edits the same preferences the manager window does,
            // so both surfaces share one queue and one revision. The reply goes
            // back to the card as well: it carries the new revision, and without
            // it the card would keep quoting a stale revision and every save
            // after the first would be rejected as a conflict.
            [this](const ui::MusicPreferencesCommand& command){
                if(exiting)return;
                const auto reply=musicPreferences.HandlePage(command);
                Post(reply);
                if(musicPage.IsOpen()){
                    // Remember it as delivered, or the next tick would see a
                    // difference and push the same frame again.
                    musicPagePreferencesSent=reply;
                    musicPage.Post(reply);
                }
                Wake();
            },
            // Background media is a separate session with its own revision; the
            // card's choice travels the same path as the main window's. The card
            // also has to be marked visible while it owns the surface, or the
            // session refuses its own choose/select operations.
            [this](const ui::MusicBackgroundCommand& command){
                if(exiting)return;
                if(musicPage.IsOpen())musicBackground.SetVisible(true);
                const auto reply=musicBackground.HandlePage(command,window.Handle());
                Post(reply);
                if(musicPage.IsOpen()){
                    musicPageBackgroundSent=reply;
                    musicPage.Post(reply);
                }
                Wake();
            },
            [this]{if(!exiting)Open(ManagerPage::Music);},
            [this]{PushMusicPage();}),
        notes(instance,NotesSessionOptions{this->options.notesPath,this->options.notesWidgetPath,this->options.verification,nullptr,
            Poster()}),
        musicPreferences(std::filesystem::u8path(this->options.musicPreferencesPath),files),
        musicFonts(MusicFontOptions(this->options,[this]{return SelectedFontFamilies();})),
        updateNotifications(instance,UpdateOptions(this->options,[this](bool manual){return RequestUpdate(manual);},
            [this](const update_notification::Notice& notice){return PresentUpdate(notice);}),
            [this](bool manual){return RequestUpdate(manual);},[this]{Wake();}),
        reminders(instance,files,todos->BaseDirectory().empty()?std::string{}:todos->BaseDirectory()+"/local/reminder-ledger.json",
            LocalReminderPorts{[this]{return todos->Read();},&todos->Time(),WindowsLocalTime::NowMillis,
                [this]{todos->RefreshTime();},[this](const std::string& id){return !exiting&&calendar.OpenTodo(id);},
                [this]{ReminderStatus();Wake();},
                this->options.verification ? this->options.verificationReminderAllowed : std::function<bool()>{},
                // With the dynamic island on, reminders appear in it instead of the popup.
                [this](const ReminderNotice& notice){return !exiting&&dynamicIsland.ShowNotice(notice.taskId,notice.title,notice.time);}}) {
        applicationSettings.SetUpdateObserver([this](bool manual,const application_settings::ManifestResult& result){
            if(exiting)return;
            updateNotifications.SetLanguage(applicationSettings.Preferences().language=="en");
            updateNotifications.OnCheckCompleted(manual,result);
        });
        todos->SetChanged([this]{
            if(exiting)return;
            islandTodos.Changed();
            calendar.RefreshTodos();todayTodo.Refresh();dynamicIsland.RefreshTodos();
            if(remindersStarted)reminders.Refresh();
        });
        window.ConfigureWallpaper(
            [this](const ui::WallpaperPageCommand& command){
                if(exiting)return;
                if(command.action==ui::WallpaperAction::Stats||command.action==ui::WallpaperAction::Like||
                    command.action==ui::WallpaperAction::EndStats) {
                    if(command.action!=ui::WallpaperAction::EndStats&&!wallpaper.IsOfficialId(command.id)) {
                        account.HandleWallpaper({ui::WallpaperAction::EndStats});return;
                    }
                    account.HandleWallpaper(command);
                }
                else wallpaper.HandlePage(command,window.Handle());
            },
            wallpaper.MediaDirectory(),
            [this](std::vector<std::wstring> paths){wallpaper.HandleDroppedFiles(std::move(paths));});
        window.ConfigureMusicDrop([this](std::vector<std::wstring> paths){
            if(!this->options.musicLibrary.root.empty()) {
                if(!musicLibrary.Import(std::move(paths)))LibraryFailure("busy");
            }
            else if(this->options.verification)music.ImportLocalFiles(std::move(paths));
        });
        window.ConfigureMusicFonts(musicFonts.AssetDirectory().wstring(),
            [this](std::wstring_view filename){return musicFonts.AssetSize(filename);});
        window.ConfigureThreadWake(lume::window_ids::ShellWakeMessage,[this]{DrainMusic();});
        window.ConfigureApplicationCommands([this](std::wstring_view message){
            if(exiting)return;
            if(const auto workbuddyCommand=ParseWorkBuddyCommand(message)) {
                HandleWorkBuddyCommand(*workbuddyCommand);
                return;
            }
            if(const auto quotaCommand=ParseQuotaCommand(message)) {
                HandleQuotaCommand(*quotaCommand);
                return;
            }
            if(const auto report=ParseDiagnosticReportCommand(message)) {
                if(report->action=="open-logs")OpenLogFolder();
                else if(report->action=="copy")StartDiagnosticJob(DiagnosticJob::Copy,report->note);
                else if(report->action=="self-check")StartDiagnosticJob(DiagnosticJob::Check,{});
                else StartDiagnosticJob(DiagnosticJob::Report,report->note);
                return;
            }
            if(const auto diagnosticsAction=diagnostics::ParseCommand(message)) {
                if(!diagnostics::FeatureEnabled)return;
                if(*diagnosticsAction==diagnostics::Action::Clear)diagnosticOperations.Clear();
                PostDiagnosticsState();
            } else if(const auto command=ui::ParseApplicationSettingsPageJson(message)) {
                if(command->action==ui::ApplicationSettingsAction::CheckUpdate&&updateNotifications.RequestManual()) {
                    if(!exiting)Post(applicationSettings.State());
                } else if(const auto reply=applicationSettings.HandlePage(*command,window.Handle()))Post(*reply);
            } else if(const auto accountCommand=ui::ParseAccountPageJson(message))account.HandlePage(*accountCommand,window.Handle());
            else if(const auto providerCommand=ParseMusicProviderAccountCommand(message)) {
                Post(musicAccounts.HandlePage(*providerCommand));
                musicOnline.AccountsChanged();
            }
            else if(const auto onlineCommand=ParseMusicOnlineCommand(message)) {
                Post(musicOnline.HandlePage(*onlineCommand));
            }
            else if(const auto libraryCommand=ui::ParseMusicLibraryCommand(message)) {
                LibraryCommand(*libraryCommand);
            }
            else if(const auto replies=NotesReply(message)) {
                for(const auto& reply:*replies)Post(reply);
            }
            else if(const auto reply=musicDesktop.HandleMessage(std::wstring(message))) {
                Post(*reply);
            }
            else if(const auto pageReply=musicPage.HandleMessage(std::wstring(message))) {
                Post(*pageReply);
            }
            else if(const auto systemFontsReply=music::fonts::HandleMusicSystemFonts(message)) {
                Post(*systemFontsReply);
            }
            else if(const auto fontReply=musicFonts.HandleMessage(message,window.Handle())) {
                Post(*fontReply);
            }
            else if(const auto backgroundCommand=ui::ParseMusicBackgroundPageJson(message)) {
                Post(musicBackground.HandlePage(*backgroundCommand,window.Handle()));
            }
            else if(const auto preferencesCommand=ui::ParseMusicPreferencesPageJson(message)) {
                Post(musicPreferences.HandlePage(*preferencesCommand));
            } else if(const auto musicCommand=ui::ParseMusicPageJson(message)) {
                if(musicCommand->action==ui::MusicAction::Close ||
                   (musicCommand->action==ui::MusicAction::Visibility&&!musicCommand->flag)) {
                    musicAccounts.CancelActive();musicFonts.Cancel();
                    musicOnline.Cancel();
                    musicLibrary.SetVisible(false);
                    musicBackground.CancelActive();musicBackground.SetVisible(false);
                } else if(musicCommand->action==ui::MusicAction::Open ||
                          (musicCommand->action==ui::MusicAction::Visibility&&musicCommand->flag)) {
                    musicBackground.SetVisible(true);
                    musicLibrary.SetVisible(true);
                    Post(musicBackground.State());
                }
                musicCommands.push_back(*musicCommand);
                Wake();
            }
        });
        window.ConfigureVisibility([this](bool visible){
            if(!visible){radial.CancelCapture();musicAccounts.CancelActive();musicOnline.Cancel();musicLibrary.SetVisible(false);musicFonts.Cancel();musicBackground.CancelActive();musicBackground.SetVisible(false);}
            if(visible && ready && !exiting) {PostCurrentState();updateNotifications.RetryPresentation();}
        });
    }
    MusicSessionOptions PlaybackOptions() {
        auto value=MusicOptions(options,[this]{Wake();});
        value.analysisActive=[this]{ return dynamicIsland.Running(); };
        if(!value.resolveSource && !value.resolveAudioSource)value.resolveAudioSource=[this](const auto& track,auto quality,const auto& cancelled){
            return musicOnline.ResolveAudioSource(track,quality,cancelled);
        };
        if(!value.readOnlineLyrics)value.readOnlineLyrics=[this](const auto& track,const auto& cancelled){
            return musicOnline.LoadLyrics(track,cancelled);
        };
        return value;
    }
    std::optional<std::vector<std::wstring>> NotesReply(std::wstring_view message) {
        if(!options.notesVisible)return {};
        return notes.HandleMessage(std::wstring(message));
    }
    std::optional<std::wstring> RadialReply(ui::PageCommand command) {
        if(!options.radialVisible)return {};
        return radial.HandlePage(command);
    }
    MusicOnlineOptions OnlineOptions() {
        auto value=options.musicOnline;
        value.verification=options.verification;
        if(!value.acquire)value.acquire=[this](auto provider){return musicAccounts.AcquireCredentials(provider);};
        if(!value.current)value.current=[this](const auto& lease){return musicAccounts.IsCurrent(lease);};
        value.wake=[this]{Wake();};
        value.play=[this](std::vector<music::Track> tracks,std::string id,std::string action){
            if(action=="play")music.PlayTracks(std::move(tracks),std::move(id));
            else music.EnqueueTracks(std::move(tracks),action=="insertNext");
            Wake();
        };
        return value;
    }
    MusicLibraryWorkerOptions LibraryOptions() {
        auto value=options.musicLibrary;
        value.wake=[this]{Wake();};
        return value;
    }
    MusicDesktopSessionOptions DesktopMusicOptions() {
        auto value=options.musicDesktop;
        value.verification=options.verification;
        value.notify=[this](const auto& state){if(!exiting)Post(state);};
        value.wake=[this]{Wake();};
        return value;
    }
    // The desktop card opens over the desktop, like a widget, and remembers its
    // own rectangle. Opening never blocks the shell: the page starts empty and
    // asks for a snapshot once its script is live.
    void ToggleMusicPage(){
        if(exiting)return;
        if(musicPage.IsOpen()){
            // The card owned the background surface while it was up; the main
            // window decides again next time it opens.
            musicPage.Close();
            musicBackground.SetVisible(false);
            return;
        }
        // The window is brand new on every open, so nothing the previous card
        // received may suppress this card's first send.
        musicPageLyricsKey.clear();
        musicPageLyricsSent=false;
        // A new window has received nothing yet, and the previous card's exact
        // strings must not suppress its first frame.
        musicPagePreferencesSent.clear();
        musicPageBackgroundSent.clear();
        // The card draws the background, so the session has to consider itself
        // in use before any choose/select arrives from the menu.
        musicBackground.SetVisible(true);
        if(musicPage.Open()){
            musicPage.ConfigureMusicFonts(musicFonts.AssetDirectory().wstring(),
                [this](std::wstring_view filename){return musicFonts.AssetSize(filename);});
            PushMusicPage();
        } else {
            musicBackground.SetVisible(false);
        }
    }
    // Keeps the card at the same revision as the widget and the manager window.
    void PushMusicPage(const music::MusicSnapshot& snapshot){
        if(!musicPage.IsOpen())return;
        // Playback ticks several times a second; rebuilding the background state
        // and preferences on each one would both flood the card and overwrite an
        // edit the user is still making. Only the snapshot and lyric channel are
        // per-frame; the other two follow their own revisions below.
        const auto preferencesState=musicPreferences.State();
        const auto backgroundState=musicBackground.State();
        ui::MusicPageState page;
        page.snapshot=snapshot;
        page.pageOpen=true;
        page.pageVisible=true;
        // Same expression the manager window's music-state uses, so both views
        // agree on the account line.
        page.accountSignedIn=options.music.accountSignedIn ? options.music.accountSignedIn() : false;
        musicPage.Post(ui::EncodeMusicPageState(page));
        // Lyrics are only ever attempted once the document can accept a message:
        // a send into a still-loading WebView is dropped, and marking that track
        // as delivered would leave the card blank for the whole song.
        if(snapshot.current && musicPage.Ready()){
            const auto key=std::to_string(snapshot.session)+"\n"+snapshot.current->id;
            if(musicPageLyricsKey!=key){
                // New track: clear the stage at once so the previous song's lines
                // do not keep scrolling under the new one while this fetches.
                musicPageLyricsKey=key;
                musicPageLyricsSent=false;
                musicPage.Post(ui::EncodeMusicLyrics({snapshot.session,snapshot.current->id,{}}));
            }
            if(!musicPageLyricsSent){
                const auto lines=music.CurrentLyrics();
                // The first read starts a background fetch and returns empty; the
                // completed lyrics arrive on a later tick and are sent then.
                if(!lines.empty()){
                    musicPageLyricsSent=true;
                    musicPage.Post(ui::EncodeMusicLyrics({snapshot.session,snapshot.current->id,lines}));
                }
            }
        }
        if(!musicPage.Ready())return;
        // Both of these carry a revision. Resending an unchanged one is wasted
        // work, and resending a stale one while the card is mid-edit would put
        // the value back the way it was.
        if(preferencesState!=musicPagePreferencesSent){
            musicPagePreferencesSent=preferencesState;
            musicPage.Post(preferencesState);
        }
        if(backgroundState!=musicPageBackgroundSent){
            musicPageBackgroundSent=backgroundState;
            musicPage.Post(backgroundState);
        }
    }
    void PushMusicPage(){PushMusicPage(music.Snapshot());}
    // Runs on the UI thread; only the chosen directory crosses into the worker.
    void ImportFolder() {
        if(options.musicLibrary.root.empty()){LibraryFailure("unavailable");return;}
        IFileOpenDialog* dialog=nullptr;
        if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))){
            LibraryFailure("unavailable");return;
        }
        DWORD dialogOptions=0;
        if(SUCCEEDED(dialog->GetOptions(&dialogOptions)))
            dialog->SetOptions(dialogOptions|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"选择要导入的音乐文件夹");
        std::wstring chosen;
        const auto shown=dialog->Show(window.Handle());
        // Cancelling the picker is not a failure; the page keeps its current state.
        if(SUCCEEDED(shown)){
            IShellItem* item=nullptr;
            if(SUCCEEDED(dialog->GetResult(&item))){
                PWSTR path=nullptr;
                if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path)))chosen=path;
                if(path)CoTaskMemFree(path);
                item->Release();
            }
        }
        dialog->Release();
        if(FAILED(shown)||chosen.empty()){
            if(FAILED(shown)&&shown!=static_cast<HRESULT>(0x800704C7L))LibraryFailure("unavailable");
            return;
        }
        if(!musicLibrary.Import(std::vector<std::wstring>{chosen}))LibraryFailure("busy");
    }
    // The playlist and the destination live here, not in the page: the helper
    // process is told both and nothing else crosses the boundary.
    void ResolveLxSyncPaths() {
        if(lxSyncScript.empty()) {
            std::wstring module(32768,L'\0');
            const auto length=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
            if(length&&length<module.size()) {
                module.resize(length);
                lxSyncScript=(std::filesystem::path(module).parent_path()/L"lxsync.py").wstring();
            }
        }
        if(lxSyncOutput.empty())lxSyncOutput=L"D:\\Music\\LX music";
    }
    void StartLxSync() {
        ResolveLxSyncPaths();
        if(lxSync.Running()){LibraryFailure("busy");return;}
        lxSyncActive=true;
        lxSyncState=nlohmann::json{{"active",true},{"stage","download"},{"done",0},{"total",0}};
        PublishSyncProgress();
        if(!lxSync.Start(lxSyncScript,lxSyncOutput)) {
            lxSyncActive=false;
            lxSyncState=nlohmann::json{{"active",false},{"stage","download"},{"done",0},{"total",0}};
            PublishSyncProgress();
            LibraryFailure("unavailable");
        }
    }
    void DrainLxSync() {
        try {
            ApplyLxSyncEvents();
        } catch(const std::exception& error) {
            lume::platform::windows::diag::Log("music","lx-sync error "+std::string(error.what()));
            lxSyncActive=false;
            lxSyncState=nlohmann::json{{"active",false},{"stage","download"},{"done",0},{"total",0}};
            LibraryFailure("unavailable");
        }
    }
    void ApplyLxSyncEvents() {
        for(const auto& event:lxSync.Drain()) {
            if(event.finished) {
                lxSyncActive=false;
                lxSyncState=nlohmann::json{{"active",false},
                    {"stage",event.stage.empty()?std::string("download"):event.stage},
                    {"done",event.total},{"total",event.total}};
                PublishSyncProgress();
                if(event.failed) {LibraryFailure(event.error);continue;}
                // Fresh files land on disk before the import, so the library
                // sees them in the same pass that finished the download.
                if(!musicLibrary.Import(std::vector<std::wstring>{lxSyncOutput}))LibraryFailure("busy");
                continue;
            }
            lxSyncState=nlohmann::json{{"active",true},
                {"stage",event.stage.empty()?std::string("download"):event.stage},
                {"done",event.done},{"total",event.total}};
            PublishSyncProgress();
        }
    }
    void PublishSyncProgress() {
        PublishLibraryState(Wide(musicLibraryState.dump(-1,' ',true)));
    }
    void LibraryCommand(const ui::MusicLibraryCommand& command) {
        if(command.action=="visibility"){musicLibrary.SetVisible(command.visible);return;}
        if(command.action=="import-folder"){ImportFolder();return;}
        if(command.action=="sync-lx"){StartLxSync();return;}
        if(options.musicLibrary.root.empty()){LibraryFailure("unavailable");return;}
        bool accepted=false;
        if(command.action=="snapshot")accepted=musicLibrary.Page();
        else if(command.action=="page")accepted=musicLibrary.Page(command.offset,command.limit,command.query);
        else if(command.action=="remove")accepted=musicLibrary.Remove(command.id);
        else accepted=musicLibrary.Play(command.id,command.action=="play"?MusicLibraryPlaybackAction::Play:
            command.action=="enqueue"?MusicLibraryPlaybackAction::Enqueue:MusicLibraryPlaybackAction::InsertNext);
        if(!accepted)LibraryFailure("busy");
    }
    void LibraryFailure(const std::string& error) {
        auto json=musicLibraryState;
        json["error"]=error;
        PublishLibraryState(Wide(json.dump(-1,' ',true)));
    }
    void PublishLibraryState(const std::wstring& state) {
        auto json=nlohmann::json::parse(state);
        json.erase("scope");
        json["type"]="music-library-state";
        json["revision"]=++musicLibraryRevision;
        // Progress of the LX pull travels with every library state so the page
        // keeps one authoritative snapshot instead of a second channel.
        json["sync"]=lxSyncState;
        // Artwork placeholders are native IDs, not URLs. Only explicit media leases leave the shell.
        for(auto& track:json["tracks"]) {
            const auto image=track.value("artwork",std::string{});
            if(image.rfind("http://127.0.0.1:",0)!=0)track["artwork"]="";
        }
        musicLibraryState=json;
        const auto utf8=json.dump(-1,' ',true);
        Post(lume::ui::ToWide(utf8));
    }
    std::vector<std::string> SelectedFontFamilies() {
        std::vector<std::string> result;
        const auto current=nlohmann::json::parse(musicPreferences.State());
        result.push_back(current.at("preferences").at("fontFamily").get<std::string>());
        // Deletion must preserve both durable preference generations.
        for(const auto& suffix:{std::string{},std::string{".bak"}}) {
            const auto path=options.musicPreferencesPath+suffix;
            if(options.musicPreferencesPath.empty())continue;
            std::error_code statusError;
            const auto status=std::filesystem::symlink_status(std::filesystem::u8path(path),statusError);
            if(statusError==std::errc::no_such_file_or_directory)continue;
            if(statusError || status.type()!=std::filesystem::file_type::regular)
                throw std::runtime_error("font references unavailable");
            std::string bytes;
            if(!files.ReadBounded(path,music::kMusicPreferencesMaxBytes,bytes))throw std::runtime_error("font references unavailable");
            const auto record=music::detail::ParseMusicPreferencesJson(bytes);
            music::MusicPreferences value;
            if(!music::DecodeMusicPreferences(record.at("preferences"),value))throw std::runtime_error("font references invalid");
            result.push_back(value.fontFamily);
        }
        return result;
    }
    bool RequestUpdate(bool manual){
        if(exiting||!applicationSettings.RequestUpdateCheck(manual))return false;
        Post(applicationSettings.State());return true;
    }
    bool PresentUpdate(const update_notification::Notice& notice){
        if(exiting)return false;
        // Never open/restore the manager for an asynchronous update result. A
        // user may have switched to a game since requesting a manual check.
        const auto manager=window.Handle();
        if(!notice.manual||!ready||!visible||!manager||IsIconic(manager)||
            GetAncestor(GetForegroundWindow(),GA_ROOT)!=manager)return true;
        using update_notification::NoticeKind;
        const char* kind=notice.kind==NoticeKind::Current?"current":
            notice.kind==NoticeKind::Available?"available":
            notice.kind==NoticeKind::Incompatible?"incompatible":
            notice.kind==NoticeKind::Disabled?"disabled":"error";
        const auto message=nlohmann::json{{"version",1},{"type","app-update-notice"},
            {"kind",kind},{"updateVersion",notice.kind==NoticeKind::Error?"":notice.version},
            {"error",notice.error},{"manual",notice.manual}}.dump(-1,' ',true);
        Post(lume::ui::ToWide(message));
        return !exiting;
    }
    // Every session callback that only forwards a state message to the page.
    std::function<void(const std::wstring&)> Poster(){return [this](const std::wstring& message){Post(message);Wake();};}
    // The same, and on attention brings the manager up on page.
    std::function<void(const std::wstring&,bool)> Notifier(std::optional<ManagerPage> page={}){
        return [this,page](const std::wstring& message,bool attention){Post(message);if(attention&&page)Open(*page);Wake();};
    }
    // The desktop widgets, last to first: the same order on exit and in the destructor.
    void ShutdownWidgets(){desktopIcons.Shutdown();organizer.Shutdown();weather.Shutdown();todayTodo.Shutdown();almanac.Shutdown();performance.Shutdown();spectrum.Shutdown();calendar.Shutdown();clock.Shutdown();}
    void ShutdownApplicationServices(){
        radial.Shutdown();
        dock.Shutdown();
        dynamicIsland.Shutdown();
        quota.Shutdown();
        workbuddy.Shutdown();
        // The history is written on the way out so the next launch can offer it.
        // Saving is best effort: a read-only profile must not block quitting.
        if(!clipboardPath.empty())clipboardHistory.Save(clipboardPath);
        notes.Shutdown();
        musicDesktop.Shutdown();
        musicPage.Shutdown();
        musicFonts.Shutdown();
        if(musicLibraryRestored&&!options.musicLibrary.root.empty())musicLibrary.SaveQueue(music.Snapshot());
        music.Shutdown();
        musicLibrary.Shutdown();
        musicOnline.Shutdown();
        musicAccounts.Shutdown();
        updateNotifications.Shutdown();applicationSettings.SetUpdateObserver({});
        applicationSettings.Shutdown();account.Shutdown();
        applicationSettings.FlushPreferences();
        wallpaper.Shutdown();applicationSettings.JoinShutdown();
        musicBackground.Shutdown();
    }
    ~State(){todos->SetChanged({});ShutdownApplicationServices();reminders.Shutdown();minimize.Shutdown();taskbar.Shutdown();ShutdownWidgets();}
    ReminderAppearance Appearance()const{
        const auto& p=calendar.Snapshot();ReminderAppearance out;
        const auto color=[](calendar::Rgba value){return RGB((value.rgb>>16)&255,(value.rgb>>8)&255,value.rgb&255);};
        out.foreground=color(p.textColor);out.accent=color(p.highlightColor);
        if(calendar.Handle())GetWindowRect(calendar.Handle(),&out.anchor);
        return out;
    }
    void ReminderStatus(){
        if(!ready||exiting)return;
        Post(reminders.Error()==LocalReminderError::None
            ?L"{\"version\":1,\"type\":\"reminder-status\",\"available\":true}"
            :L"{\"version\":1,\"type\":\"reminder-status\",\"available\":false}");
    }
    void Wake(){if(thread)PostThreadMessageW(thread,lume::window_ids::ShellWakeMessage,0,0);}
    void RecordDiagnosticMessage(std::string_view bytes) {diagnosticOperations.Record(bytes);}
    void Post(const std::wstring& message){
        TraceState(message);
        bool newEvent=false;
        bool mismatchChanged=false;
        if constexpr(diagnostics::FeatureEnabled) {
          const auto bytes=Utf8(message);
          if(!bytes.empty()) {
            const auto before=diagnostics.Snapshot();
            RecordDiagnosticMessage(bytes);
            const auto after=diagnostics.Snapshot();
            newEvent=LatestEventIdentity(before)!=LatestEventIdentity(after);
            mismatchChanged=before.value("mismatches",nlohmann::json::array())!=
                after.value("mismatches",nlohmann::json::array());
        } }
        window.Post(message);
        const auto observer=options.onMessage;
        if(options.verification&&observer)observer(message);
        if((newEvent||mismatchChanged)&&ready&&visible&&!exiting)PostDiagnosticsState();
        if(newEvent&&!visible&&!exiting)NotifyDiagnosticIssue();
    }
    // Each module state change that touches its health goes to the runtime
    // log, so a report shows when (and in which order) things went wrong.
    void TraceState(const std::wstring& message) {
        if(options.verification||message.size()>262144||message.find(L"-state\"")==std::wstring::npos)return;
        const auto value=nlohmann::json::parse(Utf8(message),nullptr,false);
        if(!value.is_object())return;
        const auto type=value.value("type",std::string{});
        if(type.size()<7||type.compare(type.size()-6,6,"-state")!=0||type.rfind("music",0)==0||type=="resource-snapshot")return;
        std::string signature;
        for(const auto* field:{"error","catalogError","weatherError","materialStatus","loadStatus","status","enabled","running","mode","pendingRecovery"}) {
            const auto item=value.find(field);
            if(item==value.end())continue;
            if(item->is_string())signature+=std::string(" ")+field+"="+item->get<std::string>();
            else if(item->is_boolean())signature+=std::string(" ")+field+"="+(item->get<bool>()?"true":"false");
        }
        auto& last=tracedStates[type];
        if(signature.empty()||last==signature)return;
        last=signature;
        lume::platform::windows::diag::Log("state",type+signature);
    }
    nlohmann::json ModuleStates() {
        auto modules=nlohmann::json::object();
        for(const auto& snapshot:{ui::ClockPageState(clock),calendar.State(),performance.State(),spectrum.State(),
            almanac.State(),todayTodo.State(),weather.State(),organizer.State(),dock.State(),desktopIcons.State(),
            taskbar.State(),minimize.State(),wallpaper.State(),applicationSettings.State()}) {
            const auto value=nlohmann::json::parse(Utf8(snapshot),nullptr,false);
            if(value.is_object())modules[value.value("type",std::string("unknown"))]=value;
        }
        return modules;
    }
    void PostReportState(const nlohmann::json& state) {
        auto message=state;message["version"]=1;message["type"]="diagnostic-report-state";
        const auto wide=Wide(message.dump(-1,' ',false,nlohmann::json::error_handler_t::replace));
        if(!wide.empty())window.Post(wide);
    }
    // Report, copied text and self-check are collected here (module states
    // belong to this thread) and built on one worker at a time.
    enum class DiagnosticJob { Report, Copy, Check, StartupCheck };
    struct DiagnosticJobResult {
        DiagnosticJob kind=DiagnosticJob::Report;
        DiagnosticReportResult report;
        std::string text;
        std::vector<SelfCheck> checks;
    };
    DiagnosticReportInput DiagnosticInput(const std::string& note) {
        DiagnosticReportInput input;
        input.dataDirectory=std::filesystem::u8path(options.applicationSettings.dataDirectory);
        input.logDirectory=lume::platform::windows::diag::Directory();
        wchar_t executable[MAX_PATH]{};
        if(GetModuleFileNameW(nullptr,executable,MAX_PATH))input.installDirectory=std::filesystem::path(executable).parent_path();
        input.version=lume::application_settings::CurrentVersion;
        input.note=note;
        input.modules=ModuleStates();
        return input;
    }
    void CreateReport(const std::string& note){StartDiagnosticJob(DiagnosticJob::Report,note);}
    void StartDiagnosticJob(DiagnosticJob kind,const std::string& note) {
        const bool page=kind!=DiagnosticJob::StartupCheck;
        if(reportRunning){if(page)PostReportState({{"status","working"},{"action",JobAction(kind)}});return;}
        reportRunning=true;
        if(page)PostReportState({{"status","working"},{"action",JobAction(kind)}});
        auto input=DiagnosticInput(note);
        if(reportWorker.thread.joinable())reportWorker.thread.join();
        try {
            reportWorker.thread=std::thread([this,kind,input=std::move(input)]{
                DiagnosticJobResult result;result.kind=kind;
                try {
                    if(kind==DiagnosticJob::Report)result.report=WriteDiagnosticReport(input);
                    else if(kind==DiagnosticJob::Copy)result.text=DiagnosticText(input);
                    else result.checks=RunSelfChecks(input.installDirectory);
                } catch(...) {result.report.error="DIAGNOSTIC_JOB_FAILED";}
                {std::lock_guard<std::mutex> lock(reportMutex);reportResult=std::move(result);}
                Wake();
            });
        } catch(...) {reportRunning=false;if(page)PostReportState({{"status","failed"},{"action",JobAction(kind)},{"error","DIAGNOSTIC_REPORT_THREAD"}});}
    }
    static const char* JobAction(DiagnosticJob kind) {
        return kind==DiagnosticJob::Copy?"copy":kind==DiagnosticJob::Report?"create":"self-check";
    }
    static bool CopyText(const std::string& text) {
        const auto wide=Wide(text);
        if(wide.empty()||!OpenClipboard(nullptr))return false;
        bool copied=false;
        EmptyClipboard();
        if(const auto memory=GlobalAlloc(GMEM_MOVEABLE,(wide.size()+1)*sizeof(wchar_t))) {
            if(auto* target=static_cast<wchar_t*>(GlobalLock(memory))) {
                std::copy(wide.c_str(),wide.c_str()+wide.size()+1,target);GlobalUnlock(memory);
                copied=SetClipboardData(CF_UNICODETEXT,memory)!=nullptr;
            }
            if(!copied)GlobalFree(memory);
        }
        CloseClipboard();
        return copied;
    }
    void LogChecks(const std::vector<SelfCheck>& checks) {
        for(const auto& check:checks)
            lume::platform::windows::diag::Log("self-check",std::string(CheckStatusName(check.status))+" "+check.id+": "+check.detail);
    }
    // Startup: tell the user once about each new set of problems, not every boot.
    void NotifyChecks(const std::vector<SelfCheck>& checks) {
        const auto signature=SelfCheckSignature(checks);
        const auto folder=lume::platform::windows::diag::Directory();
        if(folder.empty())return;
        const auto file=folder/L"self-check-notified.txt";
        std::string previous;
        {std::ifstream in(file,std::ios::binary);previous.assign(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());}
        if(signature==previous)return;
        {std::ofstream out(file,std::ios::binary|std::ios::trunc);out<<signature;}
        if(signature.empty()||!diagnosticTray||options.verification||exiting)return;
        std::string titles;
        for(const auto& check:checks)if(check.status==CheckStatus::Problem)titles+=(titles.empty()?"":"、")+check.title;
        diagnosticTray->ShowNotice(L"Lume 自检发现问题",Wide("“"+titles+"”可能无法正常工作。点击查看原因和解决办法。"));
    }
    void DrainReport() {
        std::optional<DiagnosticJobResult> result;
        {std::lock_guard<std::mutex> lock(reportMutex);result.swap(reportResult);}
        if(!result)return;
        reportRunning=false;
        switch(result->kind) {
        case DiagnosticJob::Report:
            if(!result->report.ok){PostReportState({{"status","failed"},{"action","create"},{"error",result->report.error}});return;}
            PostReportState({{"status","done"},{"action","create"},{"file",result->report.path.filename().u8string()}});
            // Show the zip selected in Explorer: the user only has to send it.
            if(PIDLIST_ABSOLUTE item=ILCreateFromPathW(result->report.path.c_str())){SHOpenFolderAndSelectItems(item,0,nullptr,0);ILFree(item);}
            return;
        case DiagnosticJob::Copy:
            if(CopyText(result->text))PostReportState({{"status","done"},{"action","copy"}});
            else PostReportState({{"status","failed"},{"action","copy"},{"error","DIAGNOSTIC_COPY_FAILED"}});
            return;
        case DiagnosticJob::Check:
            LogChecks(result->checks);
            PostReportState({{"status","done"},{"action","self-check"},{"checks",SelfChecksJson(result->checks)}});
            return;
        case DiagnosticJob::StartupCheck:
            LogChecks(result->checks);
            NotifyChecks(result->checks);
            return;
        }
    }
    void OpenLogFolder() {
        const auto folder=lume::platform::windows::diag::Directory();
        if(!folder.empty())ShellExecuteW(nullptr,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    }
    void NotifyDiagnosticIssue() {
        if(!diagnostics::FeatureEnabled)return;
        if(options.verification||!diagnosticTray)return;
        const auto now=GetTickCount64();
        if(lastDiagnosticNotice&&now-lastDiagnosticNotice<300000)return;
        if(diagnosticTray->ShowDiagnosticNotice(applicationSettings.Preferences().language=="en"))
            lastDiagnosticNotice=now;
    }
    void PostDiagnosticsState() {
        if(!diagnostics::FeatureEnabled)return;
        if(exiting) return;
        auto state=diagnostics.Snapshot();
        state["version"]=1;
        state["type"]="diagnostics-state";
        state["generatedAt"]=static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        const auto text=state.dump(-1,' ',true,nlohmann::json::error_handler_t::replace);
        const auto wide=Wide(text);
        if(wide.empty())return;
        window.Post(wide);
        const auto observer=options.onMessage;
        if(options.verification&&observer)observer(wide);
    }
    void ObserveNativeEffects() {
        if(!diagnostics::FeatureEnabled)return;
        if(exiting)return;
        const auto before=diagnostics.Snapshot();
        const auto dockMessage=Utf8(dock.State());
        const auto taskbarMessage=Utf8(taskbar.State());
        if(!dockMessage.empty())RecordDiagnosticMessage(dockMessage);
        if(!taskbarMessage.empty())RecordDiagnosticMessage(taskbarMessage);
        if(!dockMessage.empty()) {
            const auto dockState=nlohmann::json::parse(dockMessage,nullptr,false);
            const bool settled=dockState.is_object()&&dockState.value("enabled",false)&&
                dockState.value("loadStatus",std::string{})=="loaded"&&
                dockState.value("error",std::string{}).empty();
            if(settled&&!dockState.value("running",false)) {
                if(!dockNotRunningSince)dockNotRunningSince=GetTickCount64();
                if(GetTickCount64()-dockNotRunningSince>=10000) {
                    diagnostics.RecordMessage(diagnostics::Json{{"version",1},{"type","dock-state"},
                        {"module","dock"},{"action","observe"},{"status","degraded"},
                        {"error","dock-not-running"},{"expected","running"},{"actual","stopped"}});
                }
            } else dockNotRunningSince=0;
        }
        const auto after=diagnostics.Snapshot();
        const bool newEvent=LatestEventIdentity(before)!=LatestEventIdentity(after);
        const bool mismatchChanged=before.value("mismatches",nlohmann::json::array())!=
            after.value("mismatches",nlohmann::json::array());
        if(ready&&visible&&!exiting&&(newEvent||mismatchChanged))PostDiagnosticsState();
        if(newEvent&&!visible&&!exiting)NotifyDiagnosticIssue();
    }
    void Navigate(){
        if(!ready||!navigation)return;
        const auto page=*navigation;navigation.reset();
        if(page==ManagerPage::Home)return;
        if(page==ManagerPage::DynamicIsland){Post(L"{\"version\":1,\"type\":\"navigate\",\"page\":\"dynamicIsland\"}");return;}
        if(page==ManagerPage::Radial){
            Post(radialShortcutSettings?L"{\"version\":1,\"type\":\"navigate\",\"page\":\"radial\",\"section\":\"shortcut\"}":
                L"{\"version\":1,\"type\":\"navigate\",\"page\":\"radial\"}");
            radialShortcutSettings=false;return;
        }
        if(page==ManagerPage::MusicDesktop) {
            Post(L"{\"version\":1,\"type\":\"navigate\",\"page\":\"music\",\"section\":\"desktop\"}");return;
        }
        if(page==ManagerPage::ApplicationSettings||page==ManagerPage::Account) {
            const auto section=page==ManagerPage::Account?L"user":openDiagnosticsOnNext?(diagnostics::FeatureEnabled?L"diagnostics":L"support"):L"general";
            openDiagnosticsOnNext=false;
            Post(std::wstring(L"{\"version\":1,\"type\":\"navigate\",\"page\":\"applicationSettings\",\"section\":\"")+
                section+L"\"}");return;
        }
        const wchar_t* name=page==ManagerPage::TodayTodo?L"todayTodo":page==ManagerPage::Calendar?L"calendar":page==ManagerPage::Performance?L"performance":page==ManagerPage::Spectrum?L"spectrum":page==ManagerPage::Almanac?L"almanac":page==ManagerPage::Weather?L"weather":page==ManagerPage::Organizer?L"organizer":page==ManagerPage::Icons?L"icons":page==ManagerPage::Taskbar?L"taskbar":page==ManagerPage::MinimizeAnimation?L"minimize":page==ManagerPage::Music?L"music":page==ManagerPage::Dock?L"dock":L"clock";
        Post(std::wstring(L"{\"version\":1,\"type\":\"navigate\",\"page\":\"")+name+L"\"}");
    }
    // One sample of the clipboard per wake. Cheap when nothing changed: the
    // sequence number only moves when some application writes to it.
    void DrainClipboard(){
        const auto sequence = GetClipboardSequenceNumber();
        if(sequence == clipboardSequence) return;
        clipboardSequence = sequence;
        if(!clipboardEnabled) return;
        if(const auto entry = platform::windows::clipboard::Read()) {
            clipboardHistory.Add(entry->text);
        }
        PushShelf();
    }
    // Hands the shelf page its rows. The window never reads the clipboard or the
    // filesystem, so a repaint cannot block on another application.
    void PushShelf(){
        using namespace platform::windows;
        std::vector<IslandClipRow> clips;
        const auto rows = clipboardHistory.Rows();
        clips.reserve(rows.size());
        for(const auto& row:rows){
            IslandClipRow view;
            view.line = clipboard::ToLine(row.text, 46);
            view.pinned = row.pinned;
            view.canCopy = true;
            view.when = AgeLabel(row.at);
            clips.push_back(std::move(view));
        }
        std::vector<IslandFileRow> fileRows;
        fileRows.reserve(droppedFiles.size());
        for(std::size_t i=0;i<droppedFiles.size();++i){
            IslandFileRow view;
            view.id = static_cast<std::uint64_t>(i+1);
            view.name = NameOfPath(droppedFiles[i]);
            view.kind = KindOfPath(droppedFiles[i]);
            // A cached icon is used when it is already there; the cache is asked
            // for it asynchronously and pushes a repaint when it arrives.
            if(shelfIcons)view.icon = shelfIcons->Find(lume::text::Wide(droppedFiles[i]));
            fileRows.push_back(std::move(view));
        }
        dynamicIsland.SetShelf(std::move(clips),
            clipboardHistory.Size()?std::wstring{}:L"\u590d\u5236\u6587\u5b57\u540e\u5728\u8fd9\u91cc\u627e\u56de",
            std::move(fileRows), droppedFiles.empty()?L"\u8fd8\u6ca1\u6709\u62d6\u5165\u6587\u4ef6":std::wstring{});
    }
    // Asks the icon cache for the dropped files' icons. The cache extracts on its
    // own worker and calls back on the UI thread through the shell's wake.
    void RequestShelfIcons(){
        if(droppedFiles.empty())return;
        if(!shelfIcons){
            // The extractor runs in a disposable helper process: a broken shell
            // extension can hang that process, never this one.
            auto extractor = std::make_shared<platform::windows::OrganizerIconExtractor>();
            std::atomic_bool* cancelled = new std::atomic_bool(false);
            shelfIcons = std::make_shared<platform::windows::OrganizerIconCache>(
                [extractor, cancelled](const std::wstring& path, const std::atomic_bool& stop, unsigned size) {
                    return extractor->Read(path, stop, size);
                }, false);
            iconCancel = std::shared_ptr<std::atomic_bool>(cancelled);
        }
        // Only what the page can actually show is asked for: extracting an icon
        // for the hundredth file would cost without ever being seen. The set is
        // stable, so a second drop reuses the queued work instead of restarting
        // the worker on every event.
        constexpr int MaxIcons = 8;
        std::vector<std::wstring> paths;
        paths.reserve(static_cast<std::size_t>(std::min<int>(MaxIcons, static_cast<int>(droppedFiles.size()))));
        for(std::size_t i=0;i<droppedFiles.size()&&i<static_cast<std::size_t>(MaxIcons);++i){
            const auto wide = lume::text::Wide(droppedFiles[i]);
            // The cache only accepts absolute drive paths; anything else would
            // make SetPaths reject the whole batch.
            if(wide.size()>=3&&wide[1]==L':'&&(wide[2]==L'\\'||wide[2]==L'/'))paths.push_back(wide);
        }
        if(paths==requestedIcons)return;
        requestedIcons=paths;
        shelfIcons->SetPaths(paths,[this]{Wake();},48);
    }
    // Saves the history at the moment it changes, not only at exit: a crash or a
    // forced stop must not lose what the user just pinned or removed.
    void PersistClipboard(){
        if(!clipboardPath.empty())clipboardHistory.Save(clipboardPath);
    }
    // The extension, upper-cased, for the label under a file's icon. An unknown
    // or missing extension is left blank rather than shown as a guess.
    static std::wstring KindOfPath(const std::string& path){
        const auto wide = lume::text::Wide(path);
        const auto dot = wide.find_last_of(L'.');
        const auto slash = wide.find_last_of(L"\\/");
        if(dot==std::wstring::npos||(slash!=std::wstring::npos&&dot<slash))return {};
        auto ext = wide.substr(dot+1);
        if(ext.size()>5)return {};
        for(auto& ch:ext)ch=static_cast<wchar_t>(towupper(ch));
        return ext;
    }
    // Just the file name: the row has one line, and the folder is the same for
    // most drops. The full path stays in droppedFiles for opening.
    static std::wstring NameOfPath(const std::string& path){
        auto wide = lume::text::Wide(path);
        const auto slash = wide.find_last_of(L"\\/");
        return slash==std::wstring::npos ? wide : wide.substr(slash+1);
    }
    // "刚刚", "5 分钟前", ... kept short: the row has one line for the text.
    static std::wstring AgeLabel(std::int64_t at){
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto minutes = (now - at) / 60000;
        if(minutes < 1) return L"\u521a\u521a";
        if(minutes < 60) return std::to_wstring(minutes) + L" \u5206\u949f";
        const auto hours = minutes / 60;
        if(hours < 24) return std::to_wstring(hours) + L" \u5c0f\u65f6";
        return std::to_wstring(hours / 24) + L" \u5929";
    }
    void DrainMusic(){
        DrainClipboard();
        DrainReport();
        DrainLxSync();
        while(!musicCommands.empty()) {
            const auto command=musicCommands.front();musicCommands.pop_front();
            if(const auto state=music.HandlePage(command))Post(*state);
        }
        if(const auto state=music.DrainNotification())Post(*state);
        if(const auto state=musicAccounts.DrainNotification()){
            Post(*state);musicOnline.AccountsChanged();
        }
        if(const auto state=musicOnline.DrainNotification())Post(*state);
        for(auto& result:musicLibrary.Drain()) {
            if(!result.state.empty())PublishLibraryState(result.state);
            else if(!result.error.empty())LibraryFailure(result.error);
            if(result.restored&&!musicLibraryRestored) {
                musicLibraryRestored=result.error.empty();
                if(result.restoredQueue&&music.Snapshot().session==0)
                    music.RestoreQueue(*result.restoredQueue,true);
            }
            if(!result.error.empty())continue;
            if(result.action==MusicLibraryPlaybackAction::Play)
                music.PlayTracks(std::move(result.playTracks),std::move(result.playId),true);
            else if(result.action==MusicLibraryPlaybackAction::Enqueue||result.action==MusicLibraryPlaybackAction::InsertNext)
                music.EnqueueTracks(std::move(result.playTracks),result.action==MusicLibraryPlaybackAction::InsertNext,true);
        }
        const auto snapshot=music.Snapshot();
        musicDesktop.Update(snapshot);
        PushMusicPage(snapshot);
        dynamic_island::Media islandMedia;
        if(snapshot.current) {
            islandMedia.session=snapshot.session;islandMedia.id=snapshot.current->id;
            islandMedia.artwork=islandCovers.Resolve(*snapshot.current);
            islandMedia.title=Wide(snapshot.current->title);islandMedia.artist=Wide(snapshot.current->artist);
            islandMedia.available=snapshot.status!=music::PlaybackStatus::Error;
            islandMedia.playing=snapshot.playing;islandMedia.position=snapshot.position;
            islandMedia.duration=snapshot.current->duration;
            islandMedia.energy=static_cast<float>(snapshot.analysis.energy);
            if(dynamicIsland.Snapshot().enabled){
                const auto lines=music.CurrentLyrics();
                // Converted once per track and lyrics, not every frame.
                const auto key=snapshot.current->id+"#"+std::to_string(lines.size())+"#"+(lines.empty()?std::string():lines.front().fullText);
                if(key!=islandCuesKey){islandCuesKey=key;islandCues=LyricCuesOf(lines);}
                islandMedia.lyrics=islandCues;
            }
            // The analysis bins are linear and cover the whole band up to
            // Nyquist, so in ordinary music only the first handful carry
            // anything and the rest sit on the noise floor. Spreading the bars
            // over the bins that actually hold music, with a square-root lift,
            // is what makes the row move with the song instead of with hiss.
            constexpr std::size_t kIslandSpectrumBins = 8;
            const auto bin = [&](std::size_t n) {
                return std::min(n, music::kMusicAnalysisBins - 1);
            };
            for(std::size_t i=0;i<islandMedia.spectrum.size();++i) {
                const double at = islandMedia.spectrum.size() > 1
                    ? static_cast<double>(i) * static_cast<double>(kIslandSpectrumBins - 1) /
                          static_cast<double>(islandMedia.spectrum.size() - 1)
                    : 0.0;
                const auto low = static_cast<std::size_t>(at);
                const auto high = std::min(low + 1, kIslandSpectrumBins - 1);
                const auto blend = static_cast<float>(at - static_cast<double>(low));
                const auto value = [&](std::size_t n) { return static_cast<float>(snapshot.analysis.bins[bin(n)]); };
                const float level = value(low) * (1.f - blend) + value(high) * blend;
                islandMedia.spectrum[i] = std::min(1.f, std::sqrt(std::max(0.f, level) * 3.f));
            }
        }
        dynamicIsland.Update(std::move(islandMedia));
        // The balance worker is polled here rather than on a timer of its own:
        // DrainMusic already runs on every wake, and a finished fetch wakes the
        // loop through the same call.
        DrainQuota();
        if(musicLibraryRestored&&!options.musicLibrary.root.empty())musicLibrary.SaveQueue(snapshot);
        for(const auto& state:musicBackground.Drain())Post(state);
    }
    // Pushes a finished balance reading to the island and, when a key is stored,
    // asks for a refresh so the page is never stale for long.
    void DrainQuota(){
        if(const auto reading=quota.Drain()){
            dynamicIsland.SetQuota(std::move(*reading));
            if(!exiting){Post(dynamicIsland.State());Post(QuotaStateMessage());}
        }
        // WorkBuddy hands over the whole slot list whenever anything changed, so
        // the island and the settings page always show the same readings.
        if(const auto accounts=workbuddy.Drain()){
            PushWorkBuddy(*accounts);
            if(!exiting)Post(WorkBuddyStateMessage(*accounts));
        }
        if(quota.HasKey() && !quota.Running()) quota.Refresh(false);
        if(!workbuddy.Running()) workbuddy.Refresh(false);
    }
    // Splits every connected account into the two pages the island knows about
    // and marks which of them join the wheel rotation. The count is not fixed:
    // whatever is connected is listed.
    std::wstring WorkBuddyStateMessage(const std::vector<WorkBuddyAccount>& accounts){
        std::vector<WorkBuddySlotState> slots;
        slots.reserve(accounts.size());
        for(const auto& item:accounts){
            WorkBuddySlotState slot;
            slot.build=item.kind==platform::windows::quota::WorkBuddyKind::DesktopAi?"ai":"desktop";
            slot.id=item.id;
            slot.connected=item.connected;
            slot.label=item.label;
            slot.status=item.status;
            slot.remaining=item.credits.ok?item.credits.remaining:0.0;
            slot.plan=item.credits.plan;
            slots.push_back(std::move(slot));
        }
        return EncodeWorkBuddyState(slots,
            workbuddy.AnyConnected(platform::windows::quota::WorkBuddyKind::Desktop),
            workbuddy.AnyConnected(platform::windows::quota::WorkBuddyKind::DesktopAi));
    }
    void PushWorkBuddy(const std::vector<WorkBuddyAccount>& accounts){
        std::vector<IslandWorkBuddyAccount> desktop, ai;
        for(const auto& item:accounts){
            IslandWorkBuddyAccount view;
            view.label=item.label.empty()?"WorkBuddy":item.label;
            view.status=item.status;
            view.credits=item.credits;
            (item.kind==platform::windows::quota::WorkBuddyKind::DesktopAi?ai:desktop).push_back(std::move(view));
        }
        dynamicIsland.SetWorkBuddy(std::move(desktop),std::move(ai));
        dynamicIsland.SetWorkBuddyAvailable(workbuddy.AnyConnected(platform::windows::quota::WorkBuddyKind::Desktop),
            workbuddy.AnyConnected(platform::windows::quota::WorkBuddyKind::DesktopAi));
    }
    // The quota page's only writer. The key is saved encrypted and never echoed
    // back to the page: the reply carries whether a key exists, nothing more.
    void HandleQuotaCommand(const QuotaCommand& command){
        if(exiting)return;
        if(command.action=="set"){
            if(!quota.SetKey(command.key))quotaError="save";
            else quotaError.clear();
        } else if(command.action=="clear"){
            quota.ClearKey();
            dynamicIsland.SetQuota({});
            quotaError.clear();
        } else if(command.action=="refresh"){
            quotaError.clear();
            quota.Refresh(true);
        }
        // A saved or cleared key changes the island's page list at once, without
        // waiting for the next balance fetch to return.
        dynamicIsland.SetQuota(quota.Reading());
        Post(QuotaStateMessage());
        Wake();
    }
    std::wstring QuotaStateMessage(){
        const auto& reading=quota.Reading();
        std::string amount,currency;
        if(reading.ok&&!reading.entries.empty()){
            amount=reading.entries.front().total;
            currency=reading.entries.front().currency;
        }
        return EncodeQuotaState(quota.HasKey(),quota.Running(),reading.ok,reading.available,
            quotaError.empty()?reading.error:quotaError,amount,currency);
    }
    // The WorkBuddy settings' only writer. Connecting starts the browser
    // handshake for one build; the URL is opened here, on the UI thread, because
    // ShellExecute needs no worker and the user should see the browser at once.
    // Disconnecting names one account by the uid the API assigned, so every
    // account added under a build is independent of the others.
    void HandleWorkBuddyCommand(const WorkBuddyCommand& command){
        if(exiting)return;
        const auto kind=command.build=="ai"?platform::windows::quota::WorkBuddyKind::DesktopAi:
            platform::windows::quota::WorkBuddyKind::Desktop;
        if(command.action=="connect"){
            if(const auto url=workbuddy.Begin(kind)){
                const auto wide=std::wstring(url->begin(),url->end());
                ShellExecuteW(nullptr,L"open",wide.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            }
        } else if(command.action=="disconnect"){
            workbuddy.Disconnect(kind,command.id);
        } else if(command.action=="refresh"){
            workbuddy.Refresh(true);
        }
        const auto accounts=workbuddy.Accounts();
        PushWorkBuddy(accounts);
        Post(WorkBuddyStateMessage(accounts));
        Post(dynamicIsland.State());
        Wake();
    }
    void PostCurrentState(){
        if(!ready||exiting)return;
        Post(ui::ClockPageState(clock));
        Post(calendar.State());
        Post(performance.State());
        Post(spectrum.State());
        Post(almanac.State());
        Post(todayTodo.State());
        Post(weather.State());
        Post(organizer.State());
        if(options.radialVisible)Post(radial.State());
        Post(dock.State());
        Post(dynamicIsland.State());
        Post(desktopIcons.State());
        Post(taskbar.State());
        if(options.minimizeVisible)Post(minimize.State());
        const auto wallpaperState=wallpaper.State();
        if(!wallpaperState.empty())Post(wallpaperState);
        Post(musicBackground.State());
        Post(applicationSettings.State());
        Post(account.State());
        Post(account.WallpaperState());
        Post(music.State());
        Post(musicAccounts.State());
        Post(musicDesktop.State());
        Post(musicPage.State());
        // The quota blocks report their own state rather than belonging to the
        // island's preference record.
        Post(QuotaStateMessage());
        Post(WorkBuddyStateMessage(workbuddy.Accounts()));
        if(options.notesVisible)Post(notes.State());
        if(!options.musicLibrary.root.empty())musicLibrary.Page();
        if(!options.musicPreferencesPath.empty())Post(musicPreferences.State());
        ReminderStatus();
        PostDiagnosticsState();
    }
    void Open(ManagerPage page){
        if(exiting||(!options.radialVisible&&page==ManagerPage::Radial)||(!options.minimizeVisible&&page==ManagerPage::MinimizeAnimation))return;
        navigation=page;
        if(visible){window.Present();Navigate();}
        else {opening=true;Wake();}
    }
    void CloseManager(){radial.CancelCapture();calendar.CancelEditor();window.RequestClose();}
    void Exit(){
        if(exiting)return;
        exiting=true;updateNotifications.Shutdown();applicationSettings.SetUpdateObserver({});reminders.Shutdown();calendar.CancelEditor();
        // WM_QUIT also unwinds a native editor, which reposts it to the page loop.
        if(window.Handle())window.RequestClose();else {awaitingQuit=true;PostQuitMessage(0);}
        Wake();
    }
    void Tray(TrayAction action){
        switch(action){case TrayAction::Exit:Exit();break;
        // The self-check notice (and the hidden legacy diagnostics notice).
        case TrayAction::OpenDiagnostics:openDiagnosticsOnNext=true;Open(ManagerPage::ApplicationSettings);break;
        case TrayAction::ExportDiagnostics:CreateReport({});break;
        case TrayAction::Open:Open(ManagerPage::Home);break;}
    }
    void ClockAction(clock::Action action){
        if(exiting)return;
        if(action.type==clock::ActionType::OpenSettings){Open(ManagerPage::Clock);return;}
        const bool ok=clock.Handle(action);Post(ui::ClockPageState(clock));
        if(!ok)Open(ManagerPage::Clock);
        Wake();
    }
    void Dispatch(ui::PageCommand command){
        if(exiting)return;
        if(command==ui::PageCommand::Ready){
            ready=true;PostCurrentState();
            if(!exiting)Navigate();
            if(!exiting)updateNotifications.RetryPresentation();
            return;
        }
        if(command==ui::PageCommand::PerformanceResourceSnapshot) {
            Post(resources::EncodeResourceSnapshot(resources::SampleProcessTree(GetCurrentProcessId())));
            return;
        }
        diagnosticOperations.Begin(command,[this]{return nlohmann::json::parse(taskbar.State()).value("revision",0ULL);});
        if(const auto action=ui::ClockActionForPage(command))ClockAction(*action);
        else if(const auto message=calendar.HandlePage(command,window.Handle())){Post(*message);reminders.UpdateAppearance(Appearance());Wake();}
        else if(const auto performanceMessage=performance.HandlePage(command)){Post(*performanceMessage);Wake();}
        else if(const auto spectrumMessage=spectrum.HandlePage(command)){Post(*spectrumMessage);Wake();}
        else if(const auto todayMessage=todayTodo.HandlePage(command)){Post(*todayMessage);Wake();}
        else if(const auto almanacMessage=almanac.HandlePage(command)){Post(*almanacMessage);Wake();}
        else if(const auto weatherMessage=weather.HandlePage(command)){Post(*weatherMessage);dynamicIsland.RefreshWeather();Wake();}
        else if(const auto organizerMessage=organizer.HandlePage(command)){Post(*organizerMessage);Wake();}
        else if(const auto radialMessage=RadialReply(command)){Post(*radialMessage);Wake();}
        else if(const auto dockMessage=dock.HandlePage(command)){Post(*dockMessage);Wake();}
        else if(const auto islandMessage=dynamicIsland.HandlePage(command)){Post(*islandMessage);Wake();}
        else if(const auto iconsMessage=desktopIcons.HandlePage(command)){Post(*iconsMessage);Wake();}
        else if(const auto next=ui::TaskbarPreferencesForPage(command,taskbar.Preferences())) {
            taskbar.Apply(*next);
            // The island owns the windows that have to step aside for the taskbar
            // edge, and it has to learn the new demand before it repaints. A fully
            // hidden taskbar is meant to stay hidden, so the edge does not apply.
            dynamicIsland.SetRevealOnEdge(next->revealOnEdge &&
                next->hideBehavior == taskbar::HideBehavior::AutoHide);
            Post(taskbar.State());Wake();
        }
        else if(const auto minimizePreferences=options.minimizeVisible?ui::MinimizePreferencesForPage(command,minimize.Preferences()):std::nullopt) {
            minimize.Apply(*minimizePreferences);Wake();
        }
    }
    int Run(){
        if(ran)return 1;
        ran=true;thread=GetCurrentThreadId();MSG queue{};PeekMessageW(&queue,nullptr,WM_USER,WM_USER,PM_NOREMOVE);
        const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(initialized))return 1;
        // The wallpaper first: its player starts on its own thread and process,
        // so after sign-in it appears while the widgets are still being built.
        wallpaper.Restore();
        clock.Restore();calendar.Restore();performance.Restore();spectrum.Restore();almanac.Restore();todayTodo.Restore();weather.Restore();organizer.Restore();desktopIcons.Restore();taskbar.Restore();
        if(options.minimizeVisible)minimize.Restore();
        if(options.radialVisible)radial.Restore();
        dock.Restore();
        dynamicIsland.SetEnglish(applicationSettings.Preferences().language=="en");dynamicIsland.Restore();
        // The shelf's history is per-user state, loaded before the first frame so
        // the page opens onto what the user had last time.
        if(!clipboardPath.empty())clipboardHistory.Load(clipboardPath);
        // A tap re-copies the entry the row stands for; the host owns both the
        // clipboard and the history, so the window only reports the row.
        // A file dropped on the capsule is recorded here, not in the window: the
        // window never reads the filesystem.
        // A chip dragged away leaves through the shell's own drag machinery, so
        // the target program receives an ordinary file drop. The original file is
        // never moved: the helper offers copy and link only.
        dynamicIsland.SetFileDragOut([this](int cell){
            if(exiting||cell<0||cell>=static_cast<int>(droppedFiles.size()))return;
            std::vector<std::wstring> paths{lume::text::Wide(droppedFiles[static_cast<std::size_t>(cell)])};
            // The drag is modal: the island must hold its place until it returns.
            dynamicIsland.SetShellDragging(true);
            platform::windows::DragPathsOut(dynamicIsland.Handle(), paths);
            dynamicIsland.SetShellDragging(false);
            Wake();
        });
        dynamicIsland.SetFilesDropped([this](std::vector<std::string> paths){
            if(exiting)return;
            droppedFiles.insert(droppedFiles.end(),paths.begin(),paths.end());
            RequestShelfIcons();
            PushShelf();
        });
        dynamicIsland.SetShelfAction([this](int row,platform::windows::IslandShelfAction action){
            using platform::windows::IslandShelfAction;
            switch(action){
            case IslandShelfAction::Copy: {
                const auto rows=clipboardHistory.Rows();
                if(row<0||row>=static_cast<int>(rows.size()))return;
                platform::windows::clipboard::CopyToClipboard(rows[static_cast<std::size_t>(row)].text);
                return;
            }
            case IslandShelfAction::Pin: {
                const auto rows=clipboardHistory.Rows();
                if(row<0||row>=static_cast<int>(rows.size()))return;
                clipboardHistory.TogglePin(rows[static_cast<std::size_t>(row)].id);
                PushShelf();PersistClipboard();
                return;
            }
            case IslandShelfAction::Remove: {
                const auto rows=clipboardHistory.Rows();
                if(row<0||row>=static_cast<int>(rows.size()))return;
                clipboardHistory.Remove(rows[static_cast<std::size_t>(row)].id);
                PushShelf();PersistClipboard();
                return;
            }
            case IslandShelfAction::OpenFile:
                if(row>=0&&row<static_cast<int>(droppedFiles.size()))
                    ShellExecuteW(nullptr,L"open",lume::text::Wide(droppedFiles[static_cast<std::size_t>(row)]).c_str(),
                                  nullptr,nullptr,SW_SHOWNORMAL);
                return;
            case IslandShelfAction::RemoveFile:
                if(row>=0&&row<static_cast<int>(droppedFiles.size())){
                    droppedFiles.erase(droppedFiles.begin()+row);
                    PushShelf();
                }
                return;
            case IslandShelfAction::ClearAll:
                clipboardHistory.Clear();
                droppedFiles.clear();
                PushShelf();PersistClipboard();
                return;
            }
        });
        PushShelf();
        // Sampling on a timer rather than on a message: the shell owns no window
        // to hear WM_CLIPBOARDUPDATE on, and the sequence number makes a tick
        // that finds nothing changed almost free.
        SetTimer(nullptr,lume::window_ids::ClipboardPollTimer,700,nullptr);
        // The saved taskbar preference reaches the island before it can be toggled.
        dynamicIsland.SetRevealOnEdge(taskbar.Preferences().revealOnEdge &&
            taskbar.Preferences().hideBehavior == taskbar::HideBehavior::AutoHide);
        musicBackground.Restore();
        if(!options.musicLibrary.root.empty())musicLibrary.Restore();
        // The music card's pin holds the island in place and keeps the card above
        // other windows. The widget toggles its own state; this applies it.
        musicDesktop.SetPinHandler([this](bool pinned){
            if(exiting)return;
            musicPinned=pinned;
            dynamicIsland.SetPinned(pinned);
            if(const auto handle=musicDesktop.Handle())
                SetWindowPos(handle,pinned?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,
                    SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            Wake();
        });
        if(!options.musicDesktop.path.empty())musicDesktop.Restore();
        if(options.notesVisible)notes.Restore();
        applicationSettings.Restore();account.Restore();
        // Capture initial state only when the diagnostics feature is enabled.
        if constexpr(diagnostics::FeatureEnabled)for(const auto& snapshot:{ui::ClockPageState(clock),calendar.State(),performance.State(),spectrum.State(),
            almanac.State(),todayTodo.State(),weather.State(),organizer.State(),dock.State(),desktopIcons.State(),
            taskbar.State(),minimize.State(),wallpaper.State(),applicationSettings.State()})
            RecordDiagnosticMessage(Utf8(snapshot));
        updateNotifications.SetLanguage(applicationSettings.Preferences().language=="en");updateNotifications.Start();
        reminders.UpdateAppearance(Appearance());remindersStarted=true;reminders.Start();
        // Restack desktop widgets when Explorer replaces the desktop host after
        // they were shown (late desktop at boot, wallpaper WorkerW, Explorer restart).
        lume::platform::windows::desktop_level::Watcher desktopLevel;
        if(!options.verification)desktopLevel.Start(instance);
        ApplicationTray tray;
        const bool trayAvailable=tray.Create(instance,[this](TrayAction action){Tray(action);},
            [this](const std::wstring& callback){if(!exiting){account.HandleCallback(callback);Open(ManagerPage::Account);}},
            diagnostics::FeatureEnabled?std::function<void()>([this]{ObserveNativeEffects();}):std::function<void()>{});
        diagnosticTray=trayAvailable?&tray:nullptr;
        // The tray window lives on this UI thread: it answers the hang probe.
        if(trayAvailable&&!options.verification)lume::platform::windows::diag::StartHangWatch(tray.Window());
        if(trayAvailable&&!options.verification)StartDiagnosticJob(DiagnosticJob::StartupCheck,{});
        if(!options.initialLoginCallback.empty()) {
            account.HandleCallback(options.initialLoginCallback);
            options.initialLoginCallback.clear();Open(ManagerPage::Account);
        }
        int result=0;
        bool showManager=!options.startHidden||opening;
        do {
            if(showManager) {
                opening=false;visible=true;ready=false;dock.Refresh();
                result=window.Run(instance,[this](ui::PageCommand command){Dispatch(command);return std::wstring{};},options.verification,options.readyScript);
                ui::MusicPageCommand closeMusic;
                closeMusic.action=ui::MusicAction::Close;
                music.HandlePage(closeMusic);
                musicAccounts.CancelActive();
                musicOnline.Cancel();
                musicLibrary.SetVisible(false);
                musicFonts.Cancel();musicBackground.CancelActive();musicBackground.SetVisible(false);
                visible=false;ready=false;radial.CancelCapture();calendar.CancelEditor();dock.Refresh();
                ui::WallpaperPageCommand releasePreview;releasePreview.action=ui::WallpaperAction::EndPreview;
                wallpaper.HandlePage(releasePreview);
                account.HandleWallpaper({ui::WallpaperAction::EndStats});
            }
            // WebView Run consumes its own WM_QUIT and releases all COM ownership.
            // Retain this same native composition until its final widget closes.
            // Exit must let GetMessage consume the quit, including a native
            // editor's repost. PostQuitMessage is synthesized only after pending
            // messages, so a filtered PeekMessage is not a reliable drain.
            // Dropped animation frames are traced beside diagnostics.json.
            if(const auto slash=options.diagnosticsPath.find_last_of("\\/");slash!=std::string::npos)
                lume::platform::windows::ui_stall::SetLogPath(lume::text::Wide(options.diagnosticsPath.substr(0,slash+1)+"launchpad-frames.log"));
            while((!exiting||awaitingQuit)&&!opening&&(options.startHidden||clock.Running()||calendar.Running()||performance.Handle()||spectrum.Handle()||todayTodo.Handle()||almanac.Handle()||weather.Handle()||organizer.Handle()||radial.Running()||dock.Running()||dock.Snapshot().enabled||dynamicIsland.Running()||desktopIcons.Running()||desktopIcons.HasRecovery()||taskbar.HasWork()||
                (taskbar.Preferences().enabled&&taskbar.Preferences().transparentTaskbar)||minimize.Running()||wallpaper.HasWork()||reminders.HasWork()||applicationSettings.HasWork()||account.HasWork()||music.HasWork()||musicAccounts.HasWork()||musicOnline.HasWork()||musicLibrary.HasWork()||musicDesktop.Running()||notes.Active()||musicBackground.HasWork()||updateNotifications.HasWork())&&trayAvailable){
                MSG message{};const int got=GetMessageW(&message,nullptr,0,0);
                if(got<=0){exiting=true;awaitingQuit=false;break;}
                if(message.message==lume::window_ids::ShellWakeMessage){DrainMusic();continue;}
                if(message.message==WM_TIMER&&message.wParam==lume::window_ids::ClipboardPollTimer){DrainClipboard();continue;}
                TranslateMessage(&message);lume::platform::windows::ui_stall::Run(message);
            }
            showManager=opening;
        }while(!exiting&&opening);
        todos->SetChanged({});ShutdownApplicationServices();reminders.Shutdown();ShutdownWidgets();
        if(!minimize.Shutdown())result=1;
        if(!taskbar.Shutdown())result=1;
        diagnosticTray=nullptr;thread=0;CoUninitialize();return result;
    }
    HINSTANCE instance;
    DesktopApplicationOptions options;
    LocalSettingsFileStore files;
    diagnostics::Collector diagnostics;
    DiagnosticOperations diagnosticOperations{diagnostics};
    settings::JsonSettingsCodec codec;
    settings::SettingsStore store{files,codec};
    settings::SettingsSnapshot preferences;
    NativeClockRuntime clockRuntime;
    clock::ClockController clock;
    std::shared_ptr<TodoSession> todos;
    CalendarSession calendar;
    TodayTodoSession todayTodo;
    PerformanceSession performance;
    SpectrumSession spectrum;
    AlmanacSession almanac;
    WeatherSession weather;
    OrganizerSession organizer;
    RadialSession radial;
    DockSession dock;
    platform::windows::SystemMediaSession systemMedia;
    ExternalLyrics externalLyrics;
    std::string islandCuesKey;
    dynamic_island::LyricCues islandCues;
    std::string dockCoverKey;
    std::shared_ptr<const platform::windows::WidgetBitmap> dockCover;
    DynamicIslandSession dynamicIsland;
    // The DeepSeek balance worker. It fetches on its own thread and hands the
    // reading to DrainQuota(); the island never fetches.
    QuotaSession quota;
    // The four WorkBuddy slots. Like the DeepSeek worker it fetches on its own
    // threads and hands readings to DrainQuota(); the island never fetches.
    WorkBuddySession workbuddy;
    // The clipboard is polled from the wake loop rather than through a window
    // message: the shell owns no window of its own to receive one, and the
    // sequence number is the cheapest way to notice a change.
    // Resolved once at start-up so the file survives a restart.
    std::filesystem::path clipboardPath;
    platform::windows::clipboard::History clipboardHistory;
    // Paths dropped on the island, most recent last. Only the paths are kept:
    // the file itself is never copied or moved, so removing an entry cannot
    // destroy anything.
    std::vector<std::string> droppedFiles;
    // The music card's pin: the island stays put and the card stays above other
    // windows until it is turned off again.
    bool musicPinned = false;
    // File icons for the shelf, extracted off the UI thread: Shell extraction can
    // take a moment on a slow disk and must never stall a repaint.
    std::shared_ptr<platform::windows::OrganizerIconCache> shelfIcons;
    // What the cache was last asked for, so a repeated drop does not rebuild it.
    std::vector<std::wstring> requestedIcons;
    // Kept so the extractor's cancellation flag outlives the cache.
    std::shared_ptr<std::atomic_bool> iconCancel;
    DWORD clipboardSequence = 0;
    bool clipboardEnabled = true;
    // A save failure the store reported; cleared on the next successful action.
    std::string quotaError;
    lume::platform::windows::MusicCoverResolver islandCovers{[this]{Wake();}};
    lume::platform::windows::CalendarTodoEditor islandEditor;
    IslandTodoAdapter islandTodos{todos};
    DesktopIconsSession desktopIcons;
    TaskbarSession taskbar;
    MinimizeAnimationSession minimize;
    ApplicationSettingsSession applicationSettings;
    WallpaperSession wallpaper;
    MusicBackgroundSession musicBackground;
    AccountSession account;
    MusicSession music;
    MusicProviderAccountSession musicAccounts;
    MusicOnlineSession musicOnline;
    MusicLibraryWorker musicLibrary;
    MusicDesktopSession musicDesktop;
    MusicPageSession musicPage;
    NotesSession notes;
    std::uint64_t musicLibraryRevision=0;
    bool musicLibraryRestored=false;
    // LX playlist sync: script path, destination directory and the last
    // reported progress block. Nothing here comes from the page.
    lume::platform::windows::LxSyncRunner lxSync{[this]{Wake();}};
    std::wstring lxSyncScript,lxSyncOutput;
    bool lxSyncActive=false;
    nlohmann::json lxSyncState={{"active",false},{"stage","download"},{"done",0},{"total",0}};
    nlohmann::json musicLibraryState={{"version",1},{"type","music-library-state"},{"loaded",false},
        {"error",""},{"total",0},{"offset",0},{"limit",100},{"tracks",nlohmann::json::array()}};
    MusicPreferencesSession musicPreferences;
    MusicFontAssetSession musicFonts;
    UpdateNotificationSession updateNotifications;
    std::deque<ui::MusicPageCommand> musicCommands;
    // The track whose lyrics the desktop card last received.
    std::string musicPageLyricsKey;
    // Whether the current track's lyrics actually reached the card.
    bool musicPageLyricsSent=false;
    // The last preferences and background states delivered to the card. Both are
    // sent only when they change, so a per-frame resend cannot undo an edit.
    std::wstring musicPagePreferencesSent,musicPageBackgroundSent;
    LocalReminderRuntime reminders;
    WebViewPageWindow window;
    std::optional<ManagerPage> navigation;
    ApplicationTray* diagnosticTray=nullptr;
    ULONGLONG lastDiagnosticNotice=0;
    bool openDiagnosticsOnNext=false;
    ULONGLONG dockNotRunningSince=0;
    bool radialShortcutSettings=false;
    DWORD thread=0;
    bool visible=false,ready=false,opening=false,exiting=false,ran=false,awaitingQuit=false,remindersStarted=false;
    std::map<std::string,std::string> tracedStates;
    std::mutex reportMutex;
    std::optional<DiagnosticJobResult> reportResult;
    bool reportRunning=false;
    // Last member: joined first on destruction, while everything it uses lives.
    struct Worker {std::thread thread;~Worker(){if(thread.joinable())thread.join();}} reportWorker;
};
DesktopApplication::DesktopApplication(HINSTANCE instance,DesktopApplicationOptions options):state_(std::make_unique<State>(instance,std::move(options))){}
DesktopApplication::~DesktopApplication()=default;
int DesktopApplication::Run(){return state_->Run();}
void DesktopApplication::Dispatch(ui::PageCommand command){state_->Dispatch(command);}
void DesktopApplication::Open(ManagerPage page){state_->Open(page);}
void DesktopApplication::CloseManager(){state_->CloseManager();}
void DesktopApplication::Exit(){state_->Exit();}
void DesktopApplication::OnTray(TrayAction action){state_->Tray(action);}
bool DesktopApplication::ClockRunning()const{return state_->clock.Running();}
bool DesktopApplication::CalendarRunning()const{return state_->calendar.Running();}
HWND DesktopApplication::CalendarHandle()const{return state_->calendar.Handle();}
HWND DesktopApplication::ManagerHandle()const{return state_->window.Handle();}
bool DesktopApplication::ManagerReady()const{return state_->ready&&state_->window.Handle();}
}
int RunDesktopApplication(HINSTANCE instance){
    const auto launch=lume::platform::windows::CurrentShellLaunch();
    if(launch.kind==lume::platform::windows::ShellLaunchKind::Invalid)return 2;
    HANDLE singleton=CreateMutexW(nullptr,TRUE,L"Local\\LumeNativeShell1021");
    if(!singleton)return 1;
    if(GetLastError()==ERROR_ALREADY_EXISTS){
        HWND existing=FindWindowW(lume::window_ids::TrayClass,nullptr);
        if(launch.kind==lume::platform::windows::ShellLaunchKind::LoginCallback) {
            const auto deadline=GetTickCount64()+2000;
            while(!existing&&GetTickCount64()<deadline){Sleep(40);existing=FindWindowW(lume::window_ids::TrayClass,nullptr);}
            const bool forwarded=lume::platform::windows::ForwardLoginActivation(existing,launch.callback);
            CloseHandle(singleton);return forwarded?0:2;
        }
        if(existing&&launch.kind!=lume::platform::windows::ShellLaunchKind::TrayOnly)PostMessageW(existing,lume::window_ids::TrayOpenMessage,0,0);
        CloseHandle(singleton);return 0;
    }
    lume::shell::DesktopApplicationOptions options;
    // Before anything resolves a data path: honour a folder chosen in Settings.
    std::string dataMoveError;
    lume::platform::windows::ApplyPendingDataMove(dataMoveError);
    if(const auto dataRoot=lume::platform::windows::GetUserDataRootPath();!dataRoot.empty())
        lume::platform::windows::diag::Initialize(std::filesystem::u8path(dataRoot)/L"Diagnostics");
    lume::platform::windows::diag::InstallCrashHandler();
    lume::platform::windows::diag::Log("app",std::string("Lume ")+lume::application_settings::CurrentVersion+" started"+
        (launch.kind==lume::platform::windows::ShellLaunchKind::TrayOnly?" at sign-in (tray only)":"")+", pid "+std::to_string(GetCurrentProcessId()));
    // The environment once per run, off the startup path.
    std::thread([]{
        try {
            const auto system=lume::shell::DescribeSystem();
            lume::platform::windows::diag::Log("app","environment "+nlohmann::json{{"windows",system["windows"]},{"gpus",system["hardware"]["gpus"]},
                {"displays",system["displays"]},{"appearance",system["appearance"]},{"shell",system["shell"]}}.dump(-1,' ',false,nlohmann::json::error_handler_t::replace));
        } catch(...) {}
    }).detach();
    options.startHidden=launch.kind==lume::platform::windows::ShellLaunchKind::TrayOnly;
    // Temporary product visibility only; saved radial and notes files remain
    // untouched so both features can be restored for the next iteration.
    options.radialVisible=false;
    options.notesVisible=false;
    // The macOS minimize animation is likewise set aside for now: no entry, not
    // started, its preferences kept.
    options.minimizeVisible=false;
    options.initialLoginCallback=launch.callback;
    options.applicationSettings.preferencesPath=lume::platform::windows::GetApplicationSettingsPath();
    // The settings page only displays these; resolved here so the session stays
    // free of platform dependencies.
    options.applicationSettings.dataDirectory=lume::platform::windows::GetUserDataRootPath();
    options.applicationSettings.setDataDirectory=[](const std::string& chosen){
        return lume::platform::windows::SetDataDirectoryChoice(chosen);
    };
    options.applicationSettings.wallpaperDirectory=[](const std::string& preferred){
        return lume::platform::windows::GetWallpaperRootPath(preferred);
    };
    options.diagnosticsPath=lume::platform::windows::GetDiagnosticsPath();
    if(!options.applicationSettings.preferencesPath.empty())
        options.updateNotification.statePath=(std::filesystem::u8path(options.applicationSettings.preferencesPath).parent_path()/L"update-notification.json").u8string();
    options.accountOptions.storageDirectory=lume::platform::windows::GetAccountRootPath();
    options.clockPreferencesPath=lume::platform::windows::GetUiPreferencesPath();
    options.calendarPreferencesPath=lume::platform::windows::GetCalendarPreferencesPath();
    options.performancePreferencesPath=lume::platform::windows::GetPerformancePreferencesPath();
    options.spectrumPreferencesPath=lume::platform::windows::GetSpectrumPreferencesPath();
    options.almanacPreferencesPath=lume::platform::windows::GetAlmanacPreferencesPath();
    options.todayTodoPreferencesPath=lume::platform::windows::GetTodayTodoPreferencesPath();
    options.weatherPreferencesPath=lume::platform::windows::GetWeatherPreferencesPath();
    options.organizerPreferencesPath=lume::platform::windows::GetOrganizerPreferencesPath();
    options.radial.preferencesPath=lume::platform::windows::GetRadialPreferencesPath();
    options.dock.preferencesPath=lume::platform::windows::GetDockPreferencesPath();
    options.dynamicIslandPreferencesPath=lume::platform::windows::GetDynamicIslandPreferencesPath();
    options.desktopIconsPreferencesPath=lume::platform::windows::GetDesktopIconsPreferencesPath();
    options.taskbarPaths.stateFile=lume::platform::windows::GetTaskbarPreferencesPath();
    if(!options.taskbarPaths.stateFile.empty())
        options.taskbarPaths.cacheRoot=std::filesystem::u8path(options.taskbarPaths.stateFile).parent_path()/L"native-taskbar-runtime";
    std::wstring executable(32768,L'\0');
    const auto executableLength=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    if(executableLength&&executableLength<executable.size()) {
        executable.resize(executableLength);
        options.taskbarPaths.payloadRoot=std::filesystem::path(executable).parent_path()/L"taskbar-runtime";
        options.wallpaperOptions.executable=(std::filesystem::path(executable).parent_path()/L"wallpaper-runtime"/L"mpv.exe").wstring();
        options.music.executable=(std::filesystem::path(executable).parent_path()/L"LumeMusicHost.exe").wstring();
    }
    // 音符律动 melody analysis: whole-song decode and analysis on the session worker.
    options.music.analyzeScore=[](const std::string& source,const std::function<bool()>& cancelled){
        const auto audio=lume::music::DecodeScoreAudio(source,cancelled);
        return audio.mono.empty()?lume::music::MelodyAnalysis{}:lume::music::AnalyzeMelody(audio.mono,audio.rate,cancelled);
    };
    options.taskbarPaths.allowSystemEffects=true;
    options.minimizePreferencesPath=lume::platform::windows::GetMinimizePreferencesPath();
    options.musicPreferencesPath=lume::platform::windows::GetMusicPreferencesPath();
    if(!options.musicPreferencesPath.empty()) {
        const auto directory=std::filesystem::u8path(options.musicPreferencesPath).parent_path();
        options.musicLibrary.root=directory/L"music-library";
        options.musicDesktop.path=(directory/L"music-widget.json").u8string();
        options.musicPagePath=(directory/L"music-page.json").u8string();
        options.notesPath=(directory/L"notes.json").u8string();
        options.notesWidgetPath=(directory/L"notes-widget.json").u8string();
    }
    if(!options.musicPreferencesPath.empty())
        options.musicAccounts.root=std::filesystem::u8path(options.musicPreferencesPath).parent_path()/L"music-accounts";
    // Folia-style login self-check: name the broken link (DNS/IPv6/TLS/timeout/proxy).
    options.musicAccounts.diagnoseHost=[](const std::string& host){
        return lume::platform::windows::LoginVerdictToken(lume::platform::windows::DiagnoseLoginHost(host));
    };
    if(!options.musicPreferencesPath.empty())
        options.musicFonts.root=std::filesystem::u8path(options.musicPreferencesPath).parent_path()/L"music-fonts";
    options.musicBackground.root=lume::platform::windows::GetMusicBackgroundRootPath();
    options.musicBackground.statePath=lume::platform::windows::GetMusicBackgroundPreferencesPath();
    // The wallpaper folder is resolved once, before any window exists: an
    // explicit choice from Settings wins, otherwise it follows the program.
    std::string wallpaperDirectory;
    if(!options.applicationSettings.preferencesPath.empty()){
        lume::platform::windows::LocalSettingsFileStore settingsFile;
        std::string bytes;
        if(settingsFile.Read(options.applicationSettings.preferencesPath,bytes)){
            const auto decoded=lume::application_settings::DecodePreferences(bytes);
            if(decoded.error.empty())wallpaperDirectory=decoded.preferences.wallpaperDirectory;
        }
    }
    options.wallpaperOptions.root=lume::platform::windows::GetWallpaperRootPath(wallpaperDirectory);
    int result=1;
    {lume::shell::DesktopApplication app(instance,std::move(options));result=app.Run();}
    lume::platform::windows::diag::Log("app","exited, code "+std::to_string(result));
    ReleaseMutex(singleton);CloseHandle(singleton);return result;
}
