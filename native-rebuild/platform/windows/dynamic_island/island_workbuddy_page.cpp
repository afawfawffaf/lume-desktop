#include <windows.h>
#include "platform/windows/dynamic_island/island_workbuddy_page.h"
#include "platform/windows/dynamic_island/island_quota_tabs.h"
#include "platform/windows/widget_ui/text_fit.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace lume::platform::windows {
namespace {
constexpr float kHeaderY = 13;
// Two account rows share the body, one above the other like the reference puts
// its two account cards side by side; the island is wider than it is tall, so
// the rows stack and the figures stay readable.
constexpr float kRowY[2] = {64, 122};

// The account and plan names arrive as UTF-8 from the API, so they are decoded
// rather than widened byte by byte: a Chinese name would otherwise render as
// its individual bytes, which is what a plain widening produces.
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const auto needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring text(static_cast<std::size_t>(needed), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                             static_cast<int>(value.size()), text.data(), needed))
        return {};
    return text;
}

// A credit figure keeps at most two decimals and drops a trailing ".00": the
// API reports fractional credits, the island shows a readable number.
std::wstring Credits(double value) {
    if (!std::isfinite(value) || value < 0) return L"--";
    const auto rounded = std::round(value * 100) / 100;
    wchar_t buffer[32]{};
    if (std::abs(rounded - std::round(rounded)) < 0.005)
        swprintf_s(buffer, L"%.0f", std::round(rounded));
    else
        swprintf_s(buffer, L"%.2f", rounded);
    // Trim a trailing zero after the decimal point ("12.30" -> "12.3").
    std::wstring text(buffer);
    while (text.size() > 1 && text.back() == L'0') text.pop_back();
    if (!text.empty() && text.back() == L'.') text.pop_back();
    return text;
}

// The service mark: a ring with a dot, distinct from the DeepSeek page's mark so
// the two are not mistaken for each other at a glance.
void ServiceMark(WidgetCanvas& c, float cx, float cy, float size, float alpha) {
    c.Ellipse({cx - size / 2, cy - size / 2, size, size}, 0x2fbf8f, .20f * alpha);
    c.AnnularSector(cx, cy, size * .32f, size * .42f, 0, 360, 0x30d158, .95f * alpha);
    c.Ellipse({cx - size * .12f, cy - size * .12f, size * .24f, size * .24f}, 0xffffff, .85f * alpha);
}

const wchar_t* StateText(const std::string& status, bool hasCredits) {
    if (status == "waiting") return L"\u7b49\u5f85\u6d4f\u89c8\u5668\u6388\u6743";
    if (status == "fetching") return L"\u6b63\u5728\u67e5\u8be2";
    if (status == "expired") return L"\u6388\u6743\u5df2\u8fc7\u671f";
    if (status == "auth") return L"\u6388\u6743\u65e0\u6548";
    if (status == "network") return L"\u7f51\u7edc\u5931\u8d25";
    if (status == "parse") return L"\u54cd\u5e94\u4e0d\u53ef\u8bc6\u522b";
    if (status == "error") return L"\u67e5\u8be2\u5931\u8d25";
    if (hasCredits) return L"\u53ef\u7528\u79ef\u5206";
    return L"\u5c1a\u672a\u67e5\u8be2";
}

// One account: its mark, label, figure and the plan it is on.
void AccountRow(WidgetCanvas& c, const IslandWorkBuddyAccount& account, float top, float w,
                float alpha) {
    const bool ready = account.status == "ready" && account.credits.ok;
    // A hairline between the two rows, not around them: the capsule is the only
    // frame the reference uses.
    if (top > kRowY[0]) c.Line(18, top - 10, w - 18, top - 10, 0xffffff, .08f * alpha, 1);
    ServiceMark(c, 30, top + 12, 20, alpha);

    const auto label = account.label.empty() ? L"WorkBuddy" : Wide(account.label);
    c.Text(FitWeightedText(c, label, 116, 11, WidgetTextWeight::Medium),
           {46, top + 2, 116, 16}, 11, 0xffffff, .90f * alpha, WidgetTextWeight::Medium);
    c.Text(FitWeightedText(c, StateText(account.status, ready), 150, 9, WidgetTextWeight::Normal),
           {46, top + 19, 150, 14}, 9, 0xffffff, .45f * alpha);

    // The figure sits at the row's right edge, where the eye lands last.
    const auto figure = ready ? Credits(account.credits.remaining) : std::wstring(L"--");
    const float figureSize = ready ? 22.f : 18.f;
    const float width = std::min(120.f, c.MeasureText(figure, figureSize, WidgetTextWeight::Medium) + 4);
    c.Text(figure, {w - 20 - width, top + 4, width, 26}, figureSize, 0xffffff,
           (ready ? 1.f : .40f) * alpha, WidgetTextWeight::Medium, WidgetTextAlign::Right);

    // The plan name, when the API named one, under the figure.
    if (ready && !account.credits.plan.empty()) {
        c.Text(FitWeightedText(c, Wide(account.credits.plan), 120, 9, WidgetTextWeight::Normal),
               {w - 20 - 120, top + 30, 120, 13}, 9, 0xffffff, .42f * alpha,
               WidgetTextWeight::Normal, WidgetTextAlign::Right);
    }
}
}  // namespace

void DrawIslandWorkBuddyPage(WidgetCanvas& c, const std::vector<IslandWorkBuddyAccount>& accounts,
                             bool aiBuild, bool deepseekAvailable, bool siblingAvailable,
                             float w, float h, float alpha) {
    if (alpha <= .01f) return;
    const float right = w - 18;
    c.Text(L"AI \u989d\u5ea6", {18, kHeaderY, 64, 16}, 11, 0xffffff, .92f * alpha,
           WidgetTextWeight::Semibold);

    // The identical strip every provider page draws: which tab is filled and
    // which are dimmed follows from the build this page belongs to.
    DrawIslandQuotaTabs(c, w, aiBuild ? IslandQuotaTab::WorkBuddyAi : IslandQuotaTab::WorkBuddy,
                        deepseekAvailable, true, siblingAvailable, alpha);

    if (accounts.empty()) {
        // No slot is connected: say so and where to fix it, rather than drawing
        // an empty card that looks like a failure.
        c.Text(L"--", {20, 72, 52, 26}, 18, 0xffffff, .45f * alpha, WidgetTextWeight::Medium);
        c.Text(L"\u5c1a\u672a\u8fde\u63a5\u8d26\u53f7", {80, 74, w - 100, 18}, 12, 0xffffff, .62f * alpha);
        c.Text(FitWeightedText(c, L"\u8bbe\u7f6e \u2192 \u7075\u52a8\u5c9b \u2192 WorkBuddy", w - 100, 10,
                               WidgetTextWeight::Normal),
               {80, 94, w - 100, 16}, 10, 0xffffff, .38f * alpha);
        return;
    }
    for (std::size_t i = 0; i < accounts.size() && i < 2; ++i)
        AccountRow(c, accounts[i], kRowY[i], w, alpha);
    c.Line(18, h - 26, right, h - 26, 0xffffff, .10f * alpha, 1);
}

}  // namespace lume::platform::windows
