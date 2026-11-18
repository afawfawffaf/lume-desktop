#include "platform/windows/dynamic_island/island_quota_tabs.h"
#include "platform/windows/widget_ui/text_fit.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace lume::platform::windows {
namespace {
constexpr float kTextSize = 10;
constexpr float kPadding = 18;

std::wstring Label(IslandQuotaTab tab) {
    switch (tab) {
    case IslandQuotaTab::WorkBuddy: return L"WorkBuddy";
    case IslandQuotaTab::WorkBuddyAi: return L"WorkBuddy AI";
    default: return L"DeepSeek";
    }
}
} // namespace

IslandQuotaTabBox IslandQuotaTabAt(IslandQuotaTab tab, float width) {
    // The strip is centred on the page, with the AI variant offset to keep the
    // three labels from crowding each other.
    const float mid = width / 2;
    switch (tab) {
    case IslandQuotaTab::DeepSeek: return {mid - 66, 31};
    case IslandQuotaTab::WorkBuddyAi: return {mid + 70, 44};
    default: return {mid, 48};
    }
}

void DrawIslandQuotaTabs(WidgetCanvas& c, float w, IslandQuotaTab active,
                         bool deepseekAvailable, bool workbuddyAvailable,
                         bool workbuddyAiAvailable, float alpha) {
    if (alpha <= .01f) return;
    for (int i = 0; i < 3; ++i) {
        const auto tab = static_cast<IslandQuotaTab>(i);
        const auto box = IslandQuotaTabAt(tab, w);
        const bool available = tab == IslandQuotaTab::DeepSeek ? deepseekAvailable :
            tab == IslandQuotaTab::WorkBuddy ? workbuddyAvailable : workbuddyAiAvailable;
        const bool current = tab == active;
        const auto label = Label(tab);
        const float width = std::min(box.halfWidth * 2,
            c.MeasureText(label, kTextSize, WidgetTextWeight::Medium) + kPadding);
        if (current)
            c.Rounded({box.centerX - width / 2, IslandQuotaTabTop, width, IslandQuotaTabHeight},
                      9, 0x2f6bff, .85f * alpha);
        // Three states: the page you are on, a page with content, and a provider
        // that is not connected.
        const float text = current ? .98f : available ? .55f : .26f;
        c.Text(FitWeightedText(c, label, width - 12, kTextSize, WidgetTextWeight::Medium),
               {box.centerX - width / 2, IslandQuotaTabTop + 3, width, 14}, kTextSize, 0xffffff,
               text * alpha, WidgetTextWeight::Medium, WidgetTextAlign::Center);
    }
}

std::optional<IslandQuotaTab> IslandQuotaTabHit(float x, float y, float w,
                                                bool deepseekAvailable, bool workbuddyAvailable,
                                                bool workbuddyAiAvailable) {
    if (y < IslandQuotaTabTop || y >= IslandQuotaTabTop + IslandQuotaTabHeight) return std::nullopt;
    for (int i = 0; i < 3; ++i) {
        const auto tab = static_cast<IslandQuotaTab>(i);
        const auto box = IslandQuotaTabAt(tab, w);
        if (std::abs(x - box.centerX) > box.halfWidth) continue;
        const bool available = tab == IslandQuotaTab::DeepSeek ? deepseekAvailable :
            tab == IslandQuotaTab::WorkBuddy ? workbuddyAvailable : workbuddyAiAvailable;
        if (!available) return std::nullopt;
        return tab;
    }
    return std::nullopt;
}

} // namespace lume::platform::windows
