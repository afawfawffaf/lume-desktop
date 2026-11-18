#pragma once

#include "platform/windows/quota/deepseek_balance.h"
#include "platform/windows/widget_ui/widget_canvas.h"

namespace lume::platform::windows {

// The DeepSeek balance page in the island. Only the reading is passed in: the
// window never fetches, so a page draw cannot start a network call.
// The two flags say whether the sibling pages have anything to show; a provider
// with nothing to show keeps its tab dimmed and inert, and the tab strip is
// drawn by the same code for all three pages so the geometry cannot drift.
void DrawIslandQuotaPage(WidgetCanvas&, const quota::DeepSeekBalance&,
                         bool workbuddyAvailable, bool workbuddyAiAvailable,
                         float width, float height, float alpha);

}  // namespace lume::platform::windows
