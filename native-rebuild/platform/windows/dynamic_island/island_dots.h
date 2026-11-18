#pragma once

#include <cstddef>

namespace lume::platform::windows {

// The page-dot strip at the bottom of the island. It is both an indicator and a
// picker: each dot is a button that goes straight to that page. The geometry
// lives here so the drawing and the tap cannot drift apart — a dot that answers
// a few pixels away from where it is drawn reads as a broken control.
inline constexpr float DotStripHeight = 5;
inline constexpr float DotStripBottom = 9;   // from the page's bottom edge
inline constexpr float DotSelectedWidth = 14;
inline constexpr float DotWidth = 5;
inline constexpr float DotGap = 5;
inline constexpr float DotMargin = 6;        // total padding used when centring
// A tap may miss the dot itself: this is how far around it still counts.
inline constexpr float DotSlop = 6;

struct DotBox {
    float left = 0;
    float width = 0;
};

// Where each dot of `count` sits on a page `width` wide, left to right.
// `selected` widens one dot, which shifts the rest: the caller passes the same
// index it draws, so a strip in motion still hits correctly.
void DotBoxes(int count, int selected, float width, DotBox* out);
// Index of the dot a point selects, or -1 when the point is off the strip.
int DotAt(int count, int selected, float width, float height, float x, float y);

}  // namespace lume::platform::windows
