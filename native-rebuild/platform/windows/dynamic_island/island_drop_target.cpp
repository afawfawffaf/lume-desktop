#include "platform/windows/dynamic_island/island_drop_target.h"

#include <shellapi.h>

#include <algorithm>
#include <atomic>

namespace lume::platform::windows {
namespace {

// The single format this target accepts. A drag carrying text or a picture is
// refused rather than guessed at.
constexpr CLIPFORMAT kFiles = CF_HDROP;

std::vector<std::string> PathsOf(IDataObject* object) {
    std::vector<std::string> paths;
    if (!object) return paths;
    FORMATETC format{static_cast<CLIPFORMAT>(CF_HDROP), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(object->GetData(&format, &medium))) return paths;
    struct Release {
        STGMEDIUM* value;
        ~Release() { ReleaseStgMedium(value); }
    } release{&medium};
    if (!medium.hGlobal) return paths;
    const auto drop = static_cast<HDROP>(medium.hGlobal);
    const auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    paths.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const auto needed = DragQueryFileW(drop, i, nullptr, 0);
        if (!needed) continue;
        std::wstring wide(static_cast<std::size_t>(needed), L'\0');
        if (!DragQueryFileW(drop, i, wide.data(), needed + 1)) continue;
        // The path is handed on as UTF-8: the rest of the shell works in it.
        const auto bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) continue;
        std::string narrow(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), narrow.data(), bytes,
                            nullptr, nullptr);
        paths.push_back(std::move(narrow));
    }
    return paths;
}
}  // namespace

struct IslandDropTarget::Impl final : IDropTarget {
    // COM lifetime: one reference held by the island, released on Detach.
    std::atomic<long> refs{1};
    HWND hwnd = nullptr;
    Hover hover;
    OnDrop onDrop;
    bool registered = false;
    // A drag must be accepted here before WM_DROPFILES-style deltas arrive.
    bool accepted = false;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --refs;
        // The island owns exactly one reference; zero means the island is gone.
        if (!left) delete this;
        return static_cast<ULONG>(left);
    }

    bool Carries(IDataObject* object) const {
        if (!object) return false;
        FORMATETC format{static_cast<CLIPFORMAT>(CF_HDROP), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return object->QueryGetData(&format) == S_OK;
    }

    void Report(POINT point, bool over, std::size_t count) {
        if (!hover) return;
        IslandDropHover state;
        state.over = over;
        state.point = point;
        state.count = count;
        hover(state);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* object, DWORD, POINTL point, DWORD* effect) override {
        accepted = Carries(object);
        if (effect) *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (accepted) Report({point.x, point.y}, true, PathsOf(object).size());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
        if (effect) *effect = accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        // Only the position changes here: the count was resolved on enter and
        // reading the data object on every move would be wasteful.
        if (accepted && hover) {
            IslandDropHover state;
            state.over = true;
            state.point = {point.x, point.y};
            state.count = 0;
            hover(state);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        accepted = false;
        Report({0, 0}, false, 0);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* object, DWORD, POINTL point, DWORD* effect) override {
        const bool acceptedHere = accepted && Carries(object);
        accepted = false;
        if (effect) *effect = acceptedHere ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        // The hover is cleared first so the page stops drawing the outline even
        // before the host has finished recording the paths.
        Report({point.x, point.y}, false, 0);
        if (!acceptedHere || !onDrop) return S_OK;
        onDrop(PathsOf(object), POINT{point.x, point.y});
        return S_OK;
    }
};

IslandDropTarget::IslandDropTarget() : impl_(std::make_unique<Impl>()) {}
IslandDropTarget::~IslandDropTarget() { Detach(); }

bool IslandDropTarget::Attach(HWND hwnd, Hover hover, OnDrop onDrop) {
    Detach();
    if (!hwnd) return false;
    impl_->hwnd = hwnd;
    impl_->hover = std::move(hover);
    impl_->onDrop = std::move(onDrop);
    // A second reference keeps the object alive while OLE holds it; the island's
    // own reference is released in Detach.
    impl_->AddRef();
    if (FAILED(RegisterDragDrop(hwnd, impl_.get()))) {
        impl_->Release();
        impl_->hwnd = nullptr;
        return false;
    }
    impl_->registered = true;
    return true;
}

void IslandDropTarget::Detach() {
    if (!impl_ || !impl_->registered) return;
    RevokeDragDrop(impl_->hwnd);
    impl_->registered = false;
    impl_->hwnd = nullptr;
    impl_->hover = nullptr;
    impl_->onDrop = nullptr;
    impl_->Release();
}

}  // namespace lume::platform::windows
