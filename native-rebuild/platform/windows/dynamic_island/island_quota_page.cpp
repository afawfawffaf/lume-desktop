#include <windows.h>
#include "platform/windows/dynamic_island/island_quota_page.h"
#include "platform/windows/dynamic_island/island_quota_tabs.h"
#include "platform/windows/widget_ui/text_fit.h"

#include <algorithm>
#include <string>
#include <vector>

namespace lume::platform::windows {
namespace {
using quota::DeepSeekBalance;

// The page follows the same three bands as the reference design: a header with
// the service tab, a body with the figure, and the detail rows underneath.
constexpr float kHeaderY = 13;
constexpr float kBodyY = 52;
constexpr float kDetailY = 152;

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    // UTF-8 in, UTF-16 out: the balance amounts are ASCII, but the currency and
    // any future text field must not be widened byte by byte.
    const auto needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring text(static_cast<std::size_t>(needed), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                             static_cast<int>(value.size()), text.data(), needed))
        return {};
    return text;
}

// "110.00" prints as "110" and "1830.50" as "1830.5": trailing zeros after a
// decimal point drop, but a significant digit never does.
std::wstring Trimmed(std::string value) {
    if (value.find('.') == std::string::npos) return Wide(value);
    auto end = value.find_last_not_of('0');
    if (end == std::string::npos) return L"0";
    if (value[end] == '.') --end;
    return Wide(value.substr(0, end + 1));
}

std::wstring Symbol(const std::string& currency) {
    return currency == "USD" ? L"$" : L"\u00a5";
}

// The ring around the figure, as on the subscription page of the reference.
// DeepSeek reports an amount rather than a share of a cycle, so the ring is a
// frame that carries the figure instead of a progress arc.
void Ring(WidgetCanvas& c, float cx, float cy, float radius, bool live, float alpha) {
    c.AnnularSector(cx, cy, radius - 2.f, radius, 0, 360, 0x2f6bff, (live ? .95f : .35f) * alpha);
}

// A small ring-shaped service mark, drawn from primitives so the page needs no
// bundled icon: DeepSeek's whale is not reproducible this way, but a distinct
// blue ring reads as its badge beside the name.
void ServiceMark(WidgetCanvas& c, float cx, float cy, float size, float alpha) {
    c.Ellipse({cx - size / 2, cy - size / 2, size, size}, 0x2f6bff, .18f * alpha);
    c.AnnularSector(cx, cy, size * .30f, size * .40f, 0, 360, 0x4d8dff, .95f * alpha);
    c.AnnularSector(cx, cy, size * .30f, size * .40f, 200, 70, 0xffffff, .7f * alpha);
}
}  // namespace

void DrawIslandQuotaPage(WidgetCanvas& c, const DeepSeekBalance& balance,
                         bool workbuddyAvailable, bool workbuddyAiAvailable,
                         float w, float h, float alpha) {
    if (alpha <= .01f) return;
    const float right = w - 18;

    // ---- header: title, clock slot is drawn by the island chrome, so only the
    // page identity and the service tab live here.
    c.Text(L"AI \u989d\u5ea6", {18, kHeaderY, 64, 16}, 11, 0xffffff, .92f * alpha,
           WidgetTextWeight::Semibold);
    // The tab strip is shared by all three provider pages: the active tab is a
    // filled pill, the neighbours are dimmed. A page that has nothing to show is
    // dimmer still, so a tap on it reads as inert rather than broken.
    DrawIslandQuotaTabs(c, w, IslandQuotaTab::DeepSeek, true, workbuddyAvailable,
                        workbuddyAiAvailable, alpha);
    if (!balance.ok) {
        // Every failure names itself: an unknown balance must never render as
        // zero, and the user needs to know whether the key or the network failed.
        const std::wstring message =
            balance.error == "key" ? L"\u5c1a\u672a\u8bbe\u7f6e API Key" :
            balance.error == "auth" ? L"API Key \u65e0\u6548" :
            balance.error == "parse" ? L"\u54cd\u5e94\u683c\u5f0f\u4e0d\u53ef\u8bc6\u522b" :
            L"\u6682\u65e0\u6cd5\u83b7\u53d6\u4f59\u989d";
        Ring(c, 46, 84, 27, false, alpha);
        c.Text(L"--", {20, 70, 52, 28}, 18, 0xffffff, .55f * alpha,
               WidgetTextWeight::Medium, WidgetTextAlign::Center);
        c.Text(FitWeightedText(c, message, w - 100, 12, WidgetTextWeight::Normal),
               {86, 72, w - 100, 18}, 12, 0xffffff, .62f * alpha);
        c.Text(FitWeightedText(c, L"\u8bbe\u7f6e \u2192 \u7075\u52a8\u5c9b \u2192 DeepSeek \u4f59\u989d", w - 100, 10,
                               WidgetTextWeight::Normal),
               {86, 92, w - 100, 16}, 10, 0xffffff, .38f * alpha);
        return;
    }

    const auto& first = balance.entries.front();
    // The figure carries the currency sign; the digits stay the API's own.
    const auto amount = Symbol(first.currency) + Trimmed(first.total);
    // A ring beside the figure, and the service mark with its state under it:
    // the left column reads as "who and how much", the right side stays clear.
    Ring(c, 38, 88, 26, true, alpha);
    const float amountSize = 26;
    const float amountWidth = std::min(w - 96,
        c.MeasureText(amount, amountSize, WidgetTextWeight::Medium) + 4);
    c.Text(amount, {78, 70, amountWidth, 34}, amountSize, 0xffffff, alpha,
           WidgetTextWeight::Medium);

    // Service identity on the same baseline as the ring's centre.
    ServiceMark(c, 86, 116, 15, alpha);
    c.Text(L"DeepSeek", {98, 108, 80, 16}, 11, 0xffffff, .80f * alpha, WidgetTextWeight::Medium);
    c.Text(balance.available ? L"\u53ef\u7528" : L"\u4f59\u989d\u4e0d\u8db3",
           {172, 108, 100, 16}, 11, 0xffffff, balance.available ? .48f * alpha : .88f * alpha);

    // ---- detail rows: the same pairs the reference puts under the figure.
    const auto row = [&](float x, float y, const std::wstring& label, const std::wstring& value,
                         float valueAlpha) {
        c.Text(FitWeightedText(c, label, 88, 10, WidgetTextWeight::Normal),
               {x, y, 88, 14}, 10, 0xffffff, .45f * alpha);
        c.Text(FitWeightedText(c, value, 96, 11, WidgetTextWeight::Medium),
               {x, y + 14, 96, 16}, 11, 0xffffff, valueAlpha * alpha, WidgetTextWeight::Medium);
    };
    row(18, kDetailY, L"\u5145\u503c", Symbol(first.currency) + Trimmed(first.topped),
        .88f);
    row(130, kDetailY, L"\u8d60\u9001", Symbol(first.currency) + Trimmed(first.granted),
        .88f);
    // A second currency appears only when the API reported one.
    if (balance.entries.size() > 1) {
        const auto& second = balance.entries[1];
        row(242, kDetailY, Wide(second.currency) + L" \u4f59\u989d",
            Symbol(second.currency) + Trimmed(second.total), .88f);
    } else {
        row(242, kDetailY, L"\u5e01\u79cd", Wide(first.currency), .70f);
    }
    c.Line(18, h - 26, right, h - 26, 0xffffff, .10f * alpha, 1);
}

}  // namespace lume::platform::windows
