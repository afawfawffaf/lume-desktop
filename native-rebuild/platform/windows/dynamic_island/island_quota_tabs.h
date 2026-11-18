#pragma once

#include "platform/windows/widget_ui/widget_canvas.h"

#include <optional>

namespace lume::platform::windows {

// The three provider tabs shared by every quota page. Both the drawing and the
// hit test read this, so the shape a tap lands on is exactly the shape the user
// sees: two independent copies of the geometry had already drifted apart.
enum class IslandQuotaTab { DeepSeek, WorkBuddy, WorkBuddyAi };

struct IslandQuotaTabBox {
    float centerX = 0, halfWidth = 0;
};

// The box a tab occupies inside a page `width` wide, in DIP.
IslandQuotaTabBox IslandQuotaTabAt(IslandQuotaTab, float width);

// Y range of the tab strip. A tap outside it never selects a tab.
inline constexpr float IslandQuotaTabTop = 32, IslandQuotaTabHeight = 18;

// Draws the strip: `active` is the page being drawn, and a provider with nothing
// to show is dimmed further so a tap on it reads as inert rather than broken.
void DrawIslandQuotaTabs(WidgetCanvas&, float width, IslandQuotaTab active,
                         bool deepseekAvailable, bool workbuddyAvailable,
                         bool workbuddyAiAvailable, float alpha);

// Which tab, if any, a point selects. Unavailable providers are not selectable.
std::optional<IslandQuotaTab> IslandQuotaTabHit(float x, float y, float width,
                                                bool deepseekAvailable, bool workbuddyAvailable,
                                                bool workbuddyAiAvailable);

} // namespace lume::platform::windows
