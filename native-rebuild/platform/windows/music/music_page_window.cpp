#include "platform/windows/music/music_page_window.h"

#include "core/widget/widget_preferences_store.h"
#include "platform/windows/local_settings_file_store.h"
#include "platform/windows/lume_resource.h"
#include "platform/windows/lume_window_ids.h"
#include "platform/windows/material/widget_material_runtime.h"
#include "platform/windows/rounded_window_surface.h"
#include "platform/windows/webview_com_support.h"
#include "platform/windows/widget_ui/desktop_level.h"
#include "platform/windows/widget_ui/widget_snap_controller.h"
#include "vendor/nlohmann/json.hpp"
#include <WebView2.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <dwmapi.h>
#include <filesystem>
#include <shlobj.h>
#include <vector>
#include <windowsx.h>

namespace lume::platform::windows {
using namespace webview_detail;
namespace {
constexpr wchar_t kClass[] = L"LumeMusicPageWindow";
constexpr wchar_t kPageUrl[] = L"https://lume.local/music-page.html";
constexpr UINT kDeliver = WM_APP + 0x6e6;
constexpr float kCornerRadius = 22.f;
constexpr wchar_t kWebView3Id[] = L"{A0D6DF20-3B92-416D-AA0C-437A9C727857}";
constexpr wchar_t kController2Id[] = L"{c979903e-d4ca-4228-92eb-47ee3fa96eab}";
// The page is a card, not a document: it keeps room for the artwork and the
// transport row at the smallest allowed size and never covers more than a
// generous share of a large display.
constexpr int kDefaultWidth = 900, kDefaultHeight = 620;
constexpr int kMinWidth = 480, kMinHeight = 360;
constexpr int kMaxWidth = 1600, kMaxHeight = 1200;

void DesktopLevel(HWND window) { desktop_level::Place(window, false); }

std::filesystem::path ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path ProfileDirectory() {
    PWSTR raw = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) {
        result = std::filesystem::path(raw) / L"Lume" / L"WebView2";
        CoTaskMemFree(raw);
    }
    return result;
}

bool IsPageUrl(const wchar_t *url) {
    if (!url)
        return false;
    const std::wstring_view value(url);
    const std::wstring_view page(kPageUrl);
    return value == page || (value.size() > page.size() && value.substr(0, page.size()) == page && value[page.size()] == L'#');
}

// Imported lyric fonts live behind their own virtual host so the page never
// receives a filesystem path. The rules below mirror webview_page_window.cpp:
// exact 32-hex ids, pinned ancestor handles against junction swaps, and an
// immutable in-memory snapshot that matches the manifest size exactly.
constexpr wchar_t kMusicFontPrefix[] = L"https://lume-fonts.local/";
constexpr wchar_t kMusicFontFilter[] = L"https://lume-fonts.local/*";
constexpr std::uint64_t kMaxMusicFontBytes = 50ULL * 1024 * 1024;

struct FontReadHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit FontReadHandle(HANDLE handle) : value(handle) {}
    ~FontReadHandle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    FontReadHandle(const FontReadHandle &) = delete;
    FontReadHandle &operator=(const FontReadHandle &) = delete;
};

const wchar_t *MusicFontType(std::wstring_view uri, std::wstring_view &filename) {
    constexpr std::wstring_view prefix(kMusicFontPrefix);
    if (uri.size() <= prefix.size() || uri.substr(0, prefix.size()) != prefix)
        return nullptr;
    filename = uri.substr(prefix.size());
    if (filename.size() < 36 || filename.size() > 38 || filename[32] != L'.' ||
        filename.substr(0, 32).find_first_not_of(L"0123456789abcdef") != filename.npos)
        return nullptr;
    const auto extension = filename.substr(33);
    if (extension == L"ttf")
        return L"font/ttf";
    if (extension == L"otf")
        return L"font/otf";
    if (extension == L"woff")
        return L"font/woff";
    if (extension == L"woff2")
        return L"font/woff2";
    return nullptr;
}

bool FontFileMatches(HANDLE file, std::uint64_t expected) {
    BY_HANDLE_FILE_INFORMATION info{};
    return file != INVALID_HANDLE_VALUE && GetFileType(file) == FILE_TYPE_DISK &&
           GetFileInformationByHandle(file, &info) &&
           !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
           info.nNumberOfLinks == 1 &&
           ((static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) == expected;
}

ComPtr<IStream> ReadMusicFont(const std::wstring &directory, std::wstring_view filename, std::uint64_t expected) {
    if (!expected || expected > kMaxMusicFontBytes || directory.size() < 3 || directory.size() > 32700 ||
        directory.find(L'\0') != directory.npos || directory[1] != L':' ||
        (directory[2] != L'\\' && directory[2] != L'/') ||
        !((directory[0] >= L'A' && directory[0] <= L'Z') || (directory[0] >= L'a' && directory[0] <= L'z')) ||
        directory.find(L':', 2) != directory.npos)
        return {};
    const std::filesystem::path root(directory);
    for (const auto &part : root.relative_path()) {
        const auto name = part.native();
        if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ')
            return {};
    }
    std::vector<std::unique_ptr<FontReadHandle>> ancestors;
    auto holdDirectory = [&](const std::filesystem::path &path) {
        auto handle = std::make_unique<FontReadHandle>(CreateFileW(
            path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle->value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle->value, &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        ancestors.push_back(std::move(handle));
        return true;
    };
    auto path = root.root_path();
    if (!holdDirectory(path))
        return {};
    for (const auto &part : root.relative_path()) {
        path /= part;
        if (!holdDirectory(path))
            return {};
    }
    const FontReadHandle file(CreateFileW((root / std::wstring(filename)).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                          nullptr, OPEN_EXISTING,
                                          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!FontFileMatches(file.value, expected))
        return {};
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.Put())))
        return {};
    std::array<char, 65536> buffer{};
    auto remaining = expected;
    while (remaining) {
        const auto wanted = static_cast<DWORD>(std::min<std::uint64_t>(remaining, buffer.size()));
        DWORD read = 0;
        ULONG written = 0;
        if (!ReadFile(file.value, buffer.data(), wanted, &read, nullptr) || read != wanted ||
            FAILED(stream->Write(buffer.data(), read, &written)) || written != read)
            return {};
        remaining -= read;
    }
    if (!FontFileMatches(file.value, expected) || FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr)))
        return {};
    return stream;
}

// {version:1,type:"music-page-window",action:"close"|"drag"|"resize",edge}
int WindowAction(const std::wstring &json) {
    if (json.size() > 256)
        return 0;
    for (const auto ch : json)
        if (static_cast<unsigned>(ch) >= 0x80)
            return 0;
    try {
        const auto message = nlohmann::json::parse(json.begin(), json.end());
        if (!message.is_object() || message.value("version", 0) != 1 ||
            message.value("type", std::string{}) != "music-page-window" || !message.contains("action") ||
            !message.at("action").is_string())
            return 0;
        const auto action = message.at("action").get<std::string>();
        if (message.size() == 3) {
            if (action == "close")
                return HTCLOSE;
            if (action == "drag")
                return HTCAPTION;
            if (action == "settings")
                return HTHELP;
        }
        if (message.size() == 4 && action == "resize" && message.contains("edge") && message.at("edge").is_string()) {
            const auto edge = message.at("edge").get<std::string>();
            if (edge == "n") return HTTOP;
            if (edge == "ne") return HTTOPRIGHT;
            if (edge == "e") return HTRIGHT;
            if (edge == "se") return HTBOTTOMRIGHT;
            if (edge == "s") return HTBOTTOM;
            if (edge == "sw") return HTBOTTOMLEFT;
            if (edge == "w") return HTLEFT;
            if (edge == "nw") return HTTOPLEFT;
        }
    } catch (...) {}
    return 0;
}
} // namespace

struct MusicPageWindow::State : std::enable_shared_from_this<State> {
    HWND hwnd = nullptr;
    HMODULE loader = nullptr;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    MessageHandler onMessage;
    std::function<void()> onReady;
    std::function<void()> closed;
    std::function<void(const widget::Preferences &)> boundsChanged;
    MusicPageWindowOptions options;
    std::deque<std::wstring> inbox;
    WidgetMaterialRuntime material;
    WidgetSnapController snap;
    // Imported lyric fonts travel over their own virtual host: the page asks for
    // music-page.html's sibling origin and never learns a filesystem path.
    std::wstring fontDirectory;
    std::function<std::uint64_t(std::wstring_view)> fontLookup;
    EventRegistrationToken fontToken{};
    bool fontFilterRegistered = false;
    POINT moveCursor{};
    RECT moveOrigin{};
    bool moveAnchored = false;
    bool ready = false, closing = false, readyDelivered = false;
    bool transparentHost = false, transparentView = false;
    std::filesystem::path profile;

    widget::Preferences Bounds() const {
        auto result = widget::Defaults(widget::Limits{kDefaultWidth, kDefaultHeight, kMinWidth, kMinHeight, kMaxWidth, kMaxHeight, false});
        RECT rect{};
        if (hwnd && GetWindowRect(hwnd, &rect)) {
            const auto dpi = static_cast<int>(GetDpiForWindow(hwnd));
            result.x = rect.left;
            result.y = rect.top;
            result.placed = true;
            result.width = MulDiv(rect.right - rect.left, 96, dpi ? dpi : 96);
            result.height = MulDiv(rect.bottom - rect.top, 96, dpi ? dpi : 96);
        }
        return result;
    }

    void UpdateMaterial() {
        if (!hwnd || closing || !transparentHost || !transparentView)
            return;
        LumeMaterialParameters p{};
        p.size = sizeof(p);
        p.owner = hwnd;
        p.frosted = 1;
        p.opticalProfile = 1;
        p.lightForeground = 1;
        p.cornerRadius = kCornerRadius;
        // The card is a lyric surface over the desktop, not a pane in front of it:
        // with the glass at zero opacity the wallpaper behind shows through
        // unchanged, which is what "blend into the wallpaper" means. The material
        // is still attached so the app keeps its rounded alpha window; only its
        // pixels become invisible.
        p.opacityEnabled = 1;
        p.opacity = 0.f;
        BOOL animation = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0);
        p.reducedMotion = animation ? 0u : 1u;
        if (material.State() == WidgetMaterialState::Detached)
            material.Attach(p);
        else
            material.Update(p);
        PostMaterialStatus();
    }

    void PostMaterialStatus() {
        if (!ready || !view || closing)
            return;
        const auto state = transparentHost && transparentView ? material.State() : WidgetMaterialState::TransparentFallback;
        const wchar_t *status = state == WidgetMaterialState::Ready ? L"ready" :
                                state == WidgetMaterialState::Starting ? L"starting" : L"fallback";
        const std::wstring json = std::wstring(L"{\"version\":1,\"type\":\"music-page-material\",\"status\":\"") + status + L"\"}";
        view->PostWebMessageAsJson(json.c_str());
    }

    void Layout() {
        if (!controller || !hwnd)
            return;
        RECT client{};
        GetClientRect(hwnd, &client);
        controller->put_Bounds(client);
    }

    HRESULT FontResponse(ICoreWebView2WebResourceRequestedEventArgs *args, IStream *stream,
                         const wchar_t *type = L"application/octet-stream") {
        ComPtr<ICoreWebView2WebResourceResponse> response;
        const auto headers = std::wstring(L"Access-Control-Allow-Origin: https://lume.local\r\n"
                                          L"Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                                          L"Content-Type: ") +
                             type;
        const auto hr = environment->CreateWebResourceResponse(stream, stream ? 200 : 404,
                                                               stream ? L"OK" : L"Not Found", headers.c_str(),
                                                               response.Put());
        return FAILED(hr) ? hr : args->put_Response(response.Get());
    }

    HRESULT ReadFontResource(ICoreWebView2WebResourceRequestedEventArgs *args) {
        // Install a denial before callbacks/allocations: failure must not fall through.
        auto hr = FontResponse(args, nullptr);
        if (FAILED(hr))
            return hr;
        try {
            ComPtr<ICoreWebView2WebResourceRequest> request;
            LPWSTR uri = nullptr, method = nullptr;
            const auto free = [](LPWSTR &value) {
                CoTaskMemFree(value);
                value = nullptr;
            };
            if (FAILED(args->get_Request(request.Put())) || !request ||
                FAILED(request->get_Uri(&uri)) || !uri || FAILED(request->get_Method(&method)) || !method) {
                free(uri);
                free(method);
                return S_OK;
            }
            const bool get = std::wstring_view(method) == L"GET";
            std::wstring_view filename;
            const auto type = get ? MusicFontType(uri, filename) : nullptr;
            free(uri);
            free(method);
            if (!type || !fontLookup || fontDirectory.empty())
                return S_OK;
            const auto expected = fontLookup(filename);
            if (closing)
                return S_OK;
            auto stream = ReadMusicFont(fontDirectory, filename, expected);
            if (stream)
                hr = FontResponse(args, stream.Get(), type);
        } catch (...) {
            return S_OK;
        }
        return hr;
    }

    void Release() {
        closing = true;
        ready = false;
        snap.Clear();
        material.Detach();
        if (view && fontFilterRegistered) {
            view->RemoveWebResourceRequestedFilter(kMusicFontFilter, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
            fontFilterRegistered = false;
        }
        if (controller)
            controller->Close();
        controller.Reset();
        view.Reset();
        environment.Reset();
        if (loader)
            FreeLibrary(loader);
        loader = nullptr;
        inbox.clear();
    }

    void Attach(ICoreWebView2Controller *created) {
        controller.CopyFrom(created);
        if (FAILED(controller->get_CoreWebView2(view.Put())) || !view) {
            Fail();
            return;
        }
        ComPtr<ICoreWebView2_3> mapped;
        if (FAILED(view->QueryInterface(InterfaceId(kWebView3Id), reinterpret_cast<void **>(mapped.Put()))) ||
            FAILED(mapped->SetVirtualHostNameToFolderMapping(L"lume.local", (ExecutableDirectory() / L"ui").c_str(),
                                                             COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS))) {
            Fail();
            return;
        }
        // Without this the page can only render the bundled font families; imported
        // song fonts resolve through lume-fonts.local and are served as snapshots.
        const auto fontWeak = weak_from_this();
        auto fontResource = Handler<ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2 *,
                                    ICoreWebView2WebResourceRequestedEventArgs *>(
            L"{ab00b74c-15f1-4646-80e8-e76341d25d71}", [fontWeak](ICoreWebView2 *,
                                                                   ICoreWebView2WebResourceRequestedEventArgs *args) {
                const auto self = fontWeak.lock();
                return self && !self->closing ? self->ReadFontResource(args) : E_ABORT;
            });
        if (SUCCEEDED(view->add_WebResourceRequested(fontResource.Get(), &fontToken)) &&
            SUCCEEDED(view->AddWebResourceRequestedFilter(kMusicFontFilter, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL)))
            fontFilterRegistered = true;
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(view->get_Settings(settings.Put()))) {
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
        }
        ComPtr<ICoreWebView2Controller2> colored;
        transparentView = SUCCEEDED(controller->QueryInterface(InterfaceId(kController2Id), reinterpret_cast<void **>(colored.Put()))) &&
                          SUCCEEDED(colored->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{0, 0, 0, 0}));
        UpdateMaterial();
        const auto weak = weak_from_this();
        EventRegistrationToken token{};
        auto message = Handler<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2 *,
                               ICoreWebView2WebMessageReceivedEventArgs *>(
            L"{57213f19-00e6-49fa-8e07-898ea01ecbd2}", [weak](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) {
                const auto self = weak.lock();
                if (!self || self->closing)
                    return S_OK;
                LPWSTR source = nullptr, data = nullptr;
                if (SUCCEEDED(args->get_Source(&source)) && IsPageUrl(source) &&
                    SUCCEEDED(args->get_WebMessageAsJson(&data)) && data) {
                    self->ready = true;
                    self->PostMaterialStatus();
                    const auto length = wcsnlen(data, 8193);
                    if (length <= 8192 && self->inbox.size() < 64) {
                        self->inbox.emplace_back(data, length);
                        if (!PostMessageW(self->hwnd, kDeliver, 0, 0))
                            self->inbox.pop_back();
                    }
                }
                CoTaskMemFree(source);
                CoTaskMemFree(data);
                return S_OK;
            });
        view->add_WebMessageReceived(message.Get(), &token);
        auto navigated = Handler<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2 *,
                                 ICoreWebView2NavigationCompletedEventArgs *>(
            L"{30d68b7d-20d9-4752-a9ca-ec8448fbb5c1}", [weak](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) {
                const auto self = weak.lock();
                BOOL ok = FALSE;
                if (self && !self->closing && SUCCEEDED(args->get_IsSuccess(&ok)))
                    self->ready = ok != FALSE;
                if (self && self->ready)
                    self->PostMaterialStatus();
                return S_OK;
            });
        view->add_NavigationCompleted(navigated.Get(), &token);
        auto starting = Handler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2 *,
                                ICoreWebView2NavigationStartingEventArgs *>(
            L"{9adbe429-f36d-432b-9ddc-f8881fbd76e3}", [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) {
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                if (!IsPageUrl(uri))
                    args->put_Cancel(TRUE);
                CoTaskMemFree(uri);
                return S_OK;
            });
        view->add_NavigationStarting(starting.Get(), &token);
        auto windows = Handler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2 *,
                               ICoreWebView2NewWindowRequestedEventArgs *>(
            L"{d4c185fe-c81c-4989-97af-2d3fa7ab5651}", [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) {
                args->put_Handled(TRUE);
                return S_OK;
            });
        view->add_NewWindowRequested(windows.Get(), &token);
        Layout();
        controller->put_IsVisible(TRUE);
        if (FAILED(view->Navigate(kPageUrl)))
            Fail();
    }

    void Fail() {
        if (hwnd && !closing)
            MessageBoxW(hwnd, L"音乐页面无法加载，请检查 WebView2 Runtime 与安装包内的界面资源。", L"Lume 音乐",
                        MB_OK | MB_ICONWARNING);
        if (hwnd)
            DestroyWindow(hwnd);
    }

    bool Initialize() {
        const auto directory = ExecutableDirectory();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(directory / L"ui" / L"music-page.html", ec))
            return false;
        loader = LoadLibraryExW((directory / L"WebView2Loader.dll").c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader)
            return false;
        using Create = HRESULT(STDAPICALLTYPE *)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
                                                 ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
        auto address = GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions");
        Create create = nullptr;
        static_assert(sizeof(create) == sizeof(address));
        std::memcpy(&create, &address, sizeof(create));
        if (profile.empty())
            profile = ProfileDirectory();
        if (!create || profile.empty())
            return false;
        auto created = Handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment *>(
            L"{4e8a3389-c9d8-4bd2-b6b5-124fee6cc14d}", [self = shared_from_this()](HRESULT result, ICoreWebView2Environment *made) {
                if (self->closing)
                    return S_OK;
                if (FAILED(result) || !made) {
                    self->Fail();
                    return S_OK;
                }
                self->environment.CopyFrom(made);
                auto controllerReady = Handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT,
                                               ICoreWebView2Controller *>(
                    L"{6c4819f3-c9b7-4260-8127-c9f5bde7f68c}", [self](HRESULT hr, ICoreWebView2Controller *attached) {
                        if (self->closing) {
                            if (attached)
                                attached->Close();
                            return S_OK;
                        }
                        if (FAILED(hr) || !attached)
                            self->Fail();
                        else
                            self->Attach(attached);
                        return S_OK;
                    });
                if (FAILED(made->CreateCoreWebView2Controller(self->hwnd, controllerReady.Get())))
                    self->Fail();
                return S_OK;
            });
        return SUCCEEDED(create(nullptr, profile.c_str(), nullptr, created.Get()));
    }

    void Deliver() {
        while (!inbox.empty() && !closing) {
            auto message = std::move(inbox.front());
            inbox.pop_front();
            // The first message proves the page script is live and that
            // PostWebMessageAsJson is accepted, so the shell can push its frame.
            if (!readyDelivered) {
                readyDelivered = true;
                if (onReady) {
                    const auto callback = onReady;
                    try { callback(); } catch (...) {}
                }
            }
            const int action = WindowAction(message);
            if (action) {
                if (action == HTCLOSE)
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                else if (action == HTHELP) {
                    // The page asked for the manager's settings page. The window
                    // has no navigation of its own, so the shell gets the message
                    // and decides what to open.
                    if (onMessage) {
                        const auto handler = onMessage;
                        try { handler(message); } catch (...) {}
                    }
                } else {
                    POINT point{};
                    // The page's pointer event has already been released by the
                    // time this arrives; only start a native move/resize while
                    // the button is genuinely still down.
                    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) && GetCursorPos(&point)) {
                        ReleaseCapture();
                        SendMessageW(hwnd, WM_NCLBUTTONDOWN, static_cast<WPARAM>(action), MAKELPARAM(point.x, point.y));
                    }
                }
                continue;
            }
            const auto handler = onMessage;
            if (handler)
                handler(message);
        }
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto *raw = reinterpret_cast<State *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            raw = static_cast<State *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            raw->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(raw));
        }
        if (!raw)
            return DefWindowProcW(hwnd, message, wparam, lparam);
        const auto self = raw->shared_from_this();
        if (message == desktop_level::Message()) {
            // The card rests on the desktop: Explorer's own z-order passes must
            // not lift it above the manager or the dock.
            if (GetForegroundWindow() != hwnd && IsWindowVisible(hwnd))
                desktop_level::Place(hwnd, false);
            return 0;
        }
        switch (message) {
        case WM_MOUSEACTIVATE:
            // A click both focuses the page for the WebView and keeps the card
            // in its own band instead of jumping over every window.
            desktop_level::Select(hwnd);
            return MA_ACTIVATE;
        case WM_WINDOWPOSCHANGING:
            desktop_level::Changing(lparam);
            return 0;
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &point);
            RECT client{};
            GetClientRect(hwnd, &client);
            const int edge = MulDiv(8, static_cast<int>(GetDpiForWindow(hwnd)), 96);
            const bool left = point.x < edge, right = point.x >= client.right - edge;
            const bool top = point.y < edge, bottom = point.y >= client.bottom - edge;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            // The body stays a client area: the page's own pointer handlers drive
            // the move (its stage is pointer-transparent, so presses reach it).
            return HTCLIENT;
        }
        case WM_ENTERSIZEMOVE:
            widget_snap::BeginInteraction(hwnd);
            self->moveAnchored = GetWindowRect(hwnd, &self->moveOrigin) && GetCursorPos(&self->moveCursor);
            return 0;
        case WM_MOVING: {
            auto &bounds = *reinterpret_cast<RECT *>(lparam);
            POINT cursor{};
            if (self->moveAnchored && GetCursorPos(&cursor)) {
                const auto width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
                bounds.left = self->moveOrigin.left + cursor.x - self->moveCursor.x;
                bounds.top = self->moveOrigin.top + cursor.y - self->moveCursor.y;
                bounds.right = bounds.left + width;
                bounds.bottom = bounds.top + height;
            }
            self->snap.Move(bounds);
            return TRUE;
        }
        case WM_SIZING:
            self->moveAnchored = false;
            self->snap.Resize(*reinterpret_cast<RECT *>(lparam), wparam);
            return TRUE;
        case WM_EXITSIZEMOVE:
            self->moveAnchored = false;
            self->snap.Clear();
            if (self->boundsChanged) {
                const auto callback = self->boundsChanged;
                try {
                    callback(self->Bounds());
                } catch (...) {}
            }
            return 0;
        case WM_CANCELMODE:
        case WM_CAPTURECHANGED:
        case widget_snap::CancelMessage:
            self->moveAnchored = false;
            self->snap.Clear();
            return 0;
        case WM_SIZE:
            if (self->controller)
                self->controller->put_IsVisible(wparam != SIZE_MINIMIZED);
            self->Layout();
            self->UpdateMaterial();
            return 0;
        case WM_MOVE:
            if (self->controller)
                self->controller->NotifyParentWindowPositionChanged();
            self->UpdateMaterial();
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            self->UpdateMaterial();
            return 0;
        case WM_GETMINMAXINFO: {
            const auto dpi = GetDpiForWindow(hwnd);
            auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
            info->ptMinTrackSize = {MulDiv(kMinWidth, static_cast<int>(dpi ? dpi : 96), 96),
                                    MulDiv(kMinHeight, static_cast<int>(dpi ? dpi : 96), 96)};
            info->ptMaxTrackSize = {MulDiv(kMaxWidth, static_cast<int>(dpi ? dpi : 96), 96),
                                    MulDiv(kMaxHeight, static_cast<int>(dpi ? dpi : 96), 96)};
            return 0;
        }
        case WM_DPICHANGED: {
            const auto *rect = reinterpret_cast<RECT *>(lparam);
            SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            ConfigureRoundedWindow(hwnd, kCornerRadius * static_cast<float>(HIWORD(wparam)) / 96.f, true);
            if (self->moveAnchored) {
                self->moveAnchored = GetWindowRect(hwnd, &self->moveOrigin) && GetCursorPos(&self->moveCursor);
                self->snap.Clear();
            }
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(hwnd, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            return 1;
        }
        case kDeliver:
            self->Deliver();
            return 0;
        case LumeMaterialStatusChangedMessage:
            self->PostMaterialStatus();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: {
            const auto bounds = self->Bounds();
            self->Release();
            RemovePropW(hwnd, widget_snap::Capability);
            RemovePropW(hwnd, lume::window_ids::DesktopWidgetProp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->hwnd = nullptr;
            if (self->boundsChanged) {
                const auto callback = self->boundsChanged;
                try {
                    callback(bounds);
                } catch (...) {}
            }
            auto closed = std::move(self->closed);
            self->onMessage = {};
            self->onReady = {};
            self->boundsChanged = {};
            if (closed)
                closed();
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    void Post(const std::wstring &json) {
        if (ready && view && !closing)
            view->PostWebMessageAsJson(json.c_str());
    }
};

MusicPageWindow::MusicPageWindow() = default;
MusicPageWindow::~MusicPageWindow() { Close(); }

void MusicPageWindow::ConfigureMusicFonts(std::wstring fontDirectory, MusicFontAssetLookup lookup) {
    fontDirectory_ = std::move(fontDirectory);
    fontLookup_ = std::move(lookup);
    if (state_) {
        state_->fontDirectory = fontDirectory_;
        state_->fontLookup = fontLookup_;
    }
}

bool MusicPageWindow::Open(HINSTANCE instance, MessageHandler onMessage, std::function<void()> ready,
                           std::function<void()> closed,
                           std::function<void(const widget::Preferences &)> boundsChanged, const std::wstring &profile,
                           MusicPageWindowOptions options) {
    if (IsOpen()) {
        const auto hwnd = state_->hwnd;
        ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
        desktop_level::Select(hwnd);
        return true;
    }
    auto state = std::make_shared<State>();
    state->onMessage = std::move(onMessage);
    state->onReady = std::move(ready);
    state->closed = std::move(closed);
    state->boundsChanged = std::move(boundsChanged);
    state->profile = profile;
    state->options = std::move(options);
    state->fontDirectory = fontDirectory_;
    state->fontLookup = fontLookup_;

    // The saved card can come from an older build with a different limit.
    const widget::Limits limits{kDefaultWidth, kDefaultHeight, kMinWidth, kMinHeight, kMaxWidth, kMaxHeight, false};
    widget::Preferences saved = widget::Defaults(limits);
    if (!state->options.path.empty()) {
        platform::windows::LocalSettingsFileStore local;
        widget::PreferencesStore store(state->options.files ? *state->options.files : local, limits);
        const auto loaded = store.Load(state->options.path);
        if (loaded.state != widget::LoadState::Failed)
            saved = loaded.value;
    }
    if (!widget::Valid(saved, limits))
        saved = widget::Defaults(limits);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpfnWndProc = State::Proc;
    wc.lpszClassName = kClass;
    wc.style = CS_DBLCLKS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_LUME_APP));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    POINT cursor{};
    if (saved.placed)
        cursor = {saved.x, saved.y};
    else
        GetCursorPos(&cursor);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const auto area = monitor.rcWork;
    UINT dpiX = 96, dpiY = 96;
    using MonitorDpi = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
    if (const auto shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        auto address = GetProcAddress(shcore, "GetDpiForMonitor");
        MonitorDpi query = nullptr;
        std::memcpy(&query, &address, sizeof(query));
        if (query)
            query(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), 0, &dpiX, &dpiY);
        FreeLibrary(shcore);
    }
    const int width = std::min(MulDiv(saved.placed ? saved.width : kDefaultWidth, static_cast<int>(dpiX), 96),
                               static_cast<int>(area.right - area.left));
    const int height = std::min(MulDiv(saved.placed ? saved.height : kDefaultHeight, static_cast<int>(dpiY), 96),
                                static_cast<int>(area.bottom - area.top));
    const HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, wc.lpszClassName, L"Lume 音乐", WS_POPUP,
        saved.placed ? std::clamp(saved.x, static_cast<int>(area.left), static_cast<int>(area.right) - width)
                     : area.left + (area.right - area.left - width) / 2,
        saved.placed ? std::clamp(saved.y, static_cast<int>(area.top), static_cast<int>(area.bottom) - height)
                     : area.top + (area.bottom - area.top - height) / 2,
        width, height, nullptr, nullptr, instance, state.get());
    if (!hwnd)
        return false;
    SetPropW(hwnd, lume::window_ids::DesktopWidgetProp, reinterpret_cast<HANDLE>(1));
    SetPropW(hwnd, widget_snap::Capability, reinterpret_cast<HANDLE>(1));
    state->snap.Bind(hwnd);
    ConfigureRoundedWindow(hwnd, kCornerRadius * static_cast<float>(GetDpiForWindow(hwnd)) / 96.f, true);
    const int noBackdrop = 1;
    DwmSetWindowAttribute(hwnd, 38, &noBackdrop, sizeof(noBackdrop));
    const MARGINS margins{-1, -1, -1, -1};
    const bool frameReady = SUCCEEDED(DwmExtendFrameIntoClientArea(hwnd, &margins));
    // See notes_page_window.cpp: the empty region only opts the client into
    // alpha composition; LumeGlass supplies the frosted pixels.
    const HRGN alphaRegion = CreateRectRgn(0, 0, -1, -1);
    DWM_BLURBEHIND alpha{};
    alpha.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    alpha.fEnable = TRUE;
    alpha.hRgnBlur = alphaRegion;
    state->transparentHost = alphaRegion && frameReady && SUCCEEDED(DwmEnableBlurBehindWindow(hwnd, &alpha));
    if (alphaRegion)
        DeleteObject(alphaRegion);
    state_ = state;
    if (!state->Initialize()) {
        state->Fail();
        return false;
    }
    // The card belongs to the desktop, like a widget: it must not steal focus
    // from whatever the user is doing when the page finishes loading.
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    DesktopLevel(hwnd);
    return true;
}

void MusicPageWindow::RequestClose() {
    if (IsOpen())
        PostMessageW(state_->hwnd, WM_CLOSE, 0, 0);
}

void MusicPageWindow::Close() {
    if (!state_)
        return;
    const auto state = std::move(state_);
    state->closed = {};
    state->onMessage = {};
    state->onReady = {};
    state->boundsChanged = {};
    if (state->hwnd)
        DestroyWindow(state->hwnd);
    else
        state->Release();
}

void MusicPageWindow::Post(const std::wstring &json) {
    if (state_)
        state_->Post(json);
}

bool MusicPageWindow::IsOpen() const { return state_ && state_->hwnd; }

bool MusicPageWindow::Ready() const { return state_ && state_->ready; }
HWND MusicPageWindow::Handle() const { return state_ ? state_->hwnd : nullptr; }
widget::Preferences MusicPageWindow::Bounds() const { return state_ ? state_->Bounds() : widget::Preferences{}; }

void MusicPageWindow::Configure(const widget::Preferences &value) {
    if (!state_)
        return;
    const widget::Limits limits{kDefaultWidth, kDefaultHeight, kMinWidth, kMinHeight, kMaxWidth, kMaxHeight, false};
    if (!widget::Valid(value, limits) || !state_->hwnd)
        return;
    const auto dpi = static_cast<int>(GetDpiForWindow(state_->hwnd));
    SetWindowPos(state_->hwnd, nullptr, value.x, value.y, MulDiv(value.width, dpi, 96), MulDiv(value.height, dpi, 96),
                 SWP_NOZORDER | SWP_NOACTIVATE | (value.placed ? 0 : SWP_NOMOVE));
}
} // namespace lume::platform::windows
