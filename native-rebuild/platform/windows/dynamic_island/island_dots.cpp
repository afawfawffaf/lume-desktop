#include "platform/windows/dynamic_island/island_dots.h"

namespace lume::platform::windows {

void DotBoxes(int count, int selected, float width, DotBox* out) {
    if (!out || count <= 0) return;
    // Straight from the old drawing code: the strip is centred by its own total
    // width, which uses a fixed 10 per dot plus the margin.
    float x = width / 2 - (static_cast<float>(count) * 10 + DotMargin) / 2;
    for (int i = 0; i < count; ++i) {
        const float dot = i == selected ? DotSelectedWidth : DotWidth;
        out[i] = {x, dot};
        x += dot + DotGap;
    }
}

int DotAt(int count, int selected, float width, float height, float x, float y) {
    if (count < 2) return -1;  // one page has no picker
    const float top = height - DotStripBottom;
    if (y < top - DotSlop || y >= top + DotStripHeight + DotSlop) return -1;
    DotBox boxes[32]{};
    if (count > 32) return -1;
    DotBoxes(count, selected, width, boxes);
    for (int i = 0; i < count; ++i) {
        // The gap between dots belongs to neither, so a tap there does nothing
        // rather than guessing which page was meant.
        if (x >= boxes[i].left - DotSlop / 2 && x < boxes[i].left + boxes[i].width + DotSlop / 2)
            return i;
    }
    return -1;
}

}  // namespace lume::platform::windows
