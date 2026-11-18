#pragma once

#include "platform/windows/quota/workbuddy_balance.h"
#include "platform/windows/widget_ui/widget_canvas.h"

#include <string>
#include <vector>

namespace lume::platform::windows {

// One account row as the island draws it: the readings already fetched, and
// nothing that could trigger a fetch (the window never talks to the network).
struct IslandWorkBuddyAccount {
    std::string label;
    // "ready", "waiting", "fetching", "expired", "error", "none".
    std::string status;
    quota::WorkBuddyCredits credits;
};

// The WorkBuddy page of the island. Both builds share one layout; the caller
// picks which build to pass so the page can name it in the tab strip.
// `deepseekAvailable` and `workbuddyAiAvailable` describe the sibling pages, so
// every page draws the identical strip from island_quota_tabs.cpp.
void DrawIslandWorkBuddyPage(WidgetCanvas&, const std::vector<IslandWorkBuddyAccount>&,
                             bool aiBuild, bool deepseekAvailable, bool siblingAvailable,
                             float width, float height, float alpha);

}  // namespace lume::platform::windows
