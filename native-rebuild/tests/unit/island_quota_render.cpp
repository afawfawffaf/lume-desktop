// Offscreen render of the island's quota page. Not a ctest case: it writes a
// PNG so the page can be reviewed without opening the island over the desktop.
// Argument: output path (default quota-page.png).
#include "platform/windows/dynamic_island/island_quota_page.h"
#include "platform/windows/dynamic_island/island_workbuddy_page.h"
#include "platform/windows/dynamic_island/island_shelf_page.h"
#include "platform/windows/widget_ui/widget_canvas.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <vector>
#include <string>

using namespace lume;
using namespace lume::platform::windows;
using Microsoft::WRL::ComPtr;
namespace quota = lume::platform::windows::quota;

namespace {
void Save(const WidgetBitmap& bitmap, const wchar_t* path) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) return;
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return;
    if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(&frame, &options))) return;
    if (FAILED(frame->Initialize(options.Get()))) return;
    if (FAILED(frame->SetSize(bitmap.width, bitmap.height))) return;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return;
    // WIC reads the buffer as bytes; the pixels own their storage, so the cast
    // only drops constness for a call that does not write.
    auto* bytes = reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(bitmap.pixels.data()));
    const auto stride = static_cast<UINT>(bitmap.width * 4);
    if (FAILED(frame->WritePixels(bitmap.height, stride,
                                  static_cast<UINT>(bitmap.pixels.size() * 4), bytes))) return;
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return;
    std::printf("wrote %ls (%ux%u)\n", path, bitmap.width, bitmap.height);
}

quota::DeepSeekBalance Sample() {
    quota::DeepSeekBalance value;
    value.ok = true;
    value.available = true;
    value.entries = {{"CNY", "2494.82", "500.00", "1994.82"}};
    return value;
}
}  // namespace

int wmain(int, wchar_t**) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    struct Case { const wchar_t* name; quota::DeepSeekBalance balance; };
    const auto ok = Sample();
    auto noKey = quota::DeepSeekBalance{};
    noKey.error = "key";
    auto rejected = quota::DeepSeekBalance{};
    rejected.error = "auth";
    auto empty = Sample();
    empty.available = false;
    empty.entries = {{"CNY", "0.00", "0.00", "0.00"}};
    const Case cases[] = {
        {L"ok", ok},
        {L"empty", empty},
        {L"nokey", noKey},
        {L"rejected", rejected},
    };
    const float width = 404, height = 208;
    for (const auto& item : cases) {
        WidgetCanvas canvas;
        if (!canvas.Begin(width, height, 144)) continue;
        // The island paints a near-black capsule behind its pages.
        canvas.Rounded({0, 0, width, height}, 28, 0x0b0b0d, 1.f);
        DrawIslandQuotaPage(canvas, item.balance, true, true, width, height, 1.f);
        WidgetBitmap bitmap;
        if (!canvas.Finish(bitmap)) continue;
        const std::wstring path = std::wstring(L"quota-page-") + item.name + L".png";
        Save(bitmap, path.c_str());
    }

    // WorkBuddy: two connected accounts, one ready and one expired, then a
    // disconnected build so the empty state is covered too.
    {
        std::vector<IslandWorkBuddyAccount> accounts(2);
        accounts[0].label = "\u67aa\u610f";
        accounts[0].status = "ready";
        accounts[0].credits.ok = true;
        accounts[0].credits.remaining = 2494.82;
        accounts[0].credits.plan = "\u4f53\u9a8c\u7248";
        accounts[1].label = "acct-2";
        accounts[1].status = "expired";
        for (int ai = 0; ai < 2; ++ai) {
            WidgetCanvas canvas;
            if (!canvas.Begin(width, height, 144)) continue;
            canvas.Rounded({0, 0, width, height}, 28, 0x0b0b0d, 1.f);
            DrawIslandWorkBuddyPage(canvas, accounts, ai == 1, true, true, width, height, 1.f);
            WidgetBitmap bitmap;
            if (!canvas.Finish(bitmap)) continue;
            Save(bitmap, ai == 1 ? L"quota-page-wb-ai.png" : L"quota-page-wb.png");
        }
        WidgetCanvas canvas;
        if (canvas.Begin(width, height, 144)) {
            canvas.Rounded({0, 0, width, height}, 28, 0x0b0b0d, 1.f);
            DrawIslandWorkBuddyPage(canvas, {}, false, true, true, width, height, 1.f);
            WidgetBitmap bitmap;
            if (canvas.Finish(bitmap)) Save(bitmap, L"quota-page-wb-none.png");
        }
    }
    // The shelf's two tabs: four clipboard rows (one just re-copied, one
    // pinned) and four file chips.
    {
        std::vector<IslandClipRow> clips;
        const wchar_t* texts[] = {L"验证码 482913", L"SELECT * FROM todo WHERE repeat > 0",
                                  L"13800138000", L"会议纪要：周三前确认岛仔竖版方案"};
        const wchar_t* times[] = {L"09:34", L"09:33", L"09:32", L"09:31"};
        for (int i = 0; i < 4; ++i) {
            IslandClipRow row;
            row.id = static_cast<std::uint64_t>(i + 1);
            row.line = texts[i];
            row.when = times[i];
            row.pinned = i == 3;
            row.canCopy = true;
            clips.push_back(row);
        }
        std::vector<IslandFileRow> files;
        const wchar_t* names[] = {L"项目设计.pdf", L"design-preview.png", L"本周工作.docx",
                                  L"project-files.zip"};
        const wchar_t* kinds[] = {L"PDF", L"PNG", L"DOC", L"ZIP"};
        for (int i = 0; i < 4; ++i) {
            IslandFileRow row;
            row.id = static_cast<std::uint64_t>(i + 1);
            row.name = names[i];
            row.kind = kinds[i];
            files.push_back(row);
        }
        // A second page's worth of clips and files, so the scrollbar and the
        // paging arrows appear in the probe.
        for (int extra = 0; extra < 3; ++extra) {
            IslandClipRow row;
            row.id = static_cast<std::uint64_t>(10 + extra);
            row.line = L"额外条目 " + std::to_wstring(extra + 1);
            row.when = L"09:2" + std::to_wstring(extra);
            row.canCopy = true;
            clips.push_back(row);
        }
        for (int extra = 0; extra < 6; ++extra) {
            IslandFileRow row;
            row.id = static_cast<std::uint64_t>(10 + extra);
            row.name = L"额外文件" + std::to_wstring(extra + 1) + L".txt";
            row.kind = L"TXT";
            files.push_back(row);
        }
        for (int tab = 0; tab < 2; ++tab) {
            WidgetCanvas canvas;
            if (!canvas.Begin(width, height, 144)) continue;
            canvas.Rounded({0, 0, width, height}, 28, 0x0b0b0d, 1.f);
            DrawIslandShelfPage(canvas, tab == 0 ? IslandShelfTab::Clipboard : IslandShelfTab::Files,
                                clips, L"复制文字后在这里找回", files, L"还没有拖入文件",
                                tab == 0 ? 0 : -1, 0, 0, width, height, 1.f);
            WidgetBitmap bitmap;
            if (!canvas.Finish(bitmap)) continue;
            Save(bitmap, tab == 0 ? L"shelf-clip.png" : L"shelf-files.png");
        }
    }
    CoUninitialize();
    return 0;
}
