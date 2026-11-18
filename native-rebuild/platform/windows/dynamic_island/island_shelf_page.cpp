#include <windows.h>
#include "platform/windows/dynamic_island/island_shelf_page.h"
#include "platform/windows/widget_ui/text_fit.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace lume::platform::windows {
namespace {
constexpr float kMargin = 16;
constexpr float kTabHeight = 20;
// The row's trailing controls, from the right edge inwards. Fixed positions so a
// control lands where the eye last found it, whatever the text length is.
constexpr float kRemoveWidth = 18;
constexpr float kCopyWidth = 34;
constexpr float kPinWidth = 18;
constexpr float kControlGap = 6;

std::wstring Header() { return L"\u5f85\u529e\u4e0e\u7f6e\u7269"; }
std::wstring ClipboardLabel() { return L"\u526a\u8d34\u677f"; }
std::wstring FilesLabel() { return L"\u7f6e\u7269\u67b6"; }
std::wstring ClearLabel() { return L"\u6e05\u7a7a"; }
std::wstring CopiedLabel() { return L"\u5df2\u590d\u5236"; }

// A small ×, drawn from two lines so it needs no image resource.
void Cross(WidgetCanvas& c, float cx, float cy, float radius, std::uint32_t rgb, float alpha) {
    c.Line(cx - radius, cy - radius, cx + radius, cy + radius, rgb, alpha, 1.6f);
    c.Line(cx - radius, cy + radius, cx + radius, cy - radius, rgb, alpha, 1.6f);
}

// A pin: a short stem with a round head, the same shape the reference uses.
void Pin(WidgetCanvas& c, float cx, float cy, bool on, float alpha) {
    const auto rgb = on ? 0x4d8dff : 0xffffff;
    const float a = (on ? .95f : .38f) * alpha;
    c.Ellipse({cx - 3.5f, cy - 6, 7, 7}, rgb, a);
    c.Line(cx, cy - 1, cx, cy + 5, rgb, a, 1.6f);
}
}  // namespace

int IslandShelfTabAt(float x, float y, float) {
    // The pills are a fixed size rather than measured text, so a tap lands on the
    // same box whatever the label says; the drawing reads these same numbers.
    if (y < IslandShelfTabTop || y >= IslandShelfTabTop + kTabHeight) return -1;
    const float first = kMargin;
    const float second = kMargin + 58 + 8;
    if (x >= first && x < first + 58) return 0;
    if (x >= second && x < second + 58) return 1;
    return -1;
}

int IslandShelfClipRowAt(float x, float y, float width, int rows) {
    if (x < kMargin || x >= width - kMargin) return -1;
    const float local = y - IslandShelfClipTop;
    if (local < 0) return -1;
    const int index = static_cast<int>(local / IslandShelfClipRowHeight);
    if (index < 0 || index >= rows || index >= IslandShelfClipRows) return -1;
    // The last row is shorter than its slot; do not let the gap below it count.
    const float within = local - static_cast<float>(index) * IslandShelfClipRowHeight;
    if (within >= IslandShelfClipRowHeight - 6) return -1;
    return index;
}

int IslandShelfClipButtonAt(float x, float y, float width, int rows) {
    const int row = IslandShelfClipRowAt(x, y, width, rows);
    if (row < 0) return -1;
    const float top = IslandShelfClipTop + static_cast<float>(row) * IslandShelfClipRowHeight;
    if (y < top || y >= top + IslandShelfClipRowHeight - 6) return -1;
    const float removeLeft = width - kMargin - kRemoveWidth;
    if (x >= removeLeft) return 2;
    const float copyLeft = removeLeft - kControlGap - kCopyWidth;
    if (x >= copyLeft && x < copyLeft + kCopyWidth) return 1;
    const float pinLeft = copyLeft - kControlGap - kPinWidth;
    if (x >= pinLeft && x < pinLeft + kPinWidth) return 0;
    return -1;
}

float IslandShelfFileCellWidth(float width) {
    // The arrows live in their gutters; what remains is shared by the columns.
    const float room = width - IslandShelfFileArrowGutter * 2 - 4;
    return std::max(56.f, room / static_cast<float>(IslandShelfFileColumns));
}

int IslandShelfFileCellAt(float x, float y, float width, int cells) {
    const float cell = IslandShelfFileCellWidth(width);
    if (x < IslandShelfFileArrowGutter || x >= IslandShelfFileArrowGutter + cell * IslandShelfFileColumns)
        return -1;
    const float local = y - IslandShelfFileTop;
    if (local < 0) return -1;
    const int row = static_cast<int>(local / IslandShelfFileRowHeight);
    if (row < 0 || row >= IslandShelfFileRows) return -1;
    const float within = local - static_cast<float>(row) * IslandShelfFileRowHeight;
    if (within >= IslandShelfFileRowHeight - 4) return -1;
    const float cellW = IslandShelfFileCellWidth(width);
    const int column = static_cast<int>((x - IslandShelfFileArrowGutter) / cellW);
    if (column < 0 || column >= IslandShelfFileColumns) return -1;
    const int index = row * IslandShelfFileColumns + column;
    if (index < 0 || index >= cells) return -1;
    return index;
}

int IslandShelfFileButtonAt(float x, float y, float width, int cells) {
    const int cell = IslandShelfFileCellAt(x, y, width, cells);
    if (cell < 0) return -1;
    const int column = cell % IslandShelfFileColumns;
    const int row = cell / IslandShelfFileColumns;
    const float cellW = IslandShelfFileCellWidth(width);
    const float left = IslandShelfFileArrowGutter + static_cast<float>(column) * cellW;
    const float top = IslandShelfFileTop + static_cast<float>(row) * IslandShelfFileRowHeight;
    // The × sits on the icon's top-right shoulder, where the drawing puts it.
    const float iconSize = 32;
    const float iconTop = top + 2;
    const float iconLeft = left + (cellW - iconSize) / 2;
    const float cx = iconLeft + iconSize - 2;
    const float cy = iconTop + 2;
    if (std::abs(x - cx) <= 9 && std::abs(y - cy) <= 9) return 0;
    return -1;
}

bool IslandShelfClearAt(float x, float y, float width) {
    if (y < IslandShelfHeaderY - 2 || y >= IslandShelfHeaderY + 20) return false;
    const float right = width - kMargin;
    return x >= right - 40 && x < right;
}


IslandShelfScrollBox IslandShelfScrollBar(float width, int total, int visible, int offset) {
    IslandShelfScrollBox box;
    // Always present, even when everything fits: a scrollbar that appears and
    // disappears makes the row text jump sideways, and a slot that is sometimes
    // there is harder to aim at than one that always is. A full-height thumb is
    // the honest way to say "there is nothing more to scroll".
    box.needed = visible > 0 && total > 0;
    if (!box.needed) return box;
    box.x = width - IslandShelfScrollLeft - IslandShelfScrollWidth;
    box.width = IslandShelfScrollWidth;
    box.top = IslandShelfClipTop;
    box.height = static_cast<float>(visible) * IslandShelfClipRowHeight;
    // The thumb's share is the visible fraction; never smaller than a grip, and
    // all of the track when there is nothing to scroll.
    const float share = total <= visible ? 1.f : static_cast<float>(visible) / static_cast<float>(total);
    box.thumbHeight = std::max(18.f, box.height * share);
    const float span = box.height - box.thumbHeight;
    const int maxOffset = std::max(1, total - visible);
    const float at = static_cast<float>(std::clamp(offset, 0, maxOffset)) / static_cast<float>(maxOffset);
    box.thumbTop = box.top + span * at;
    return box;
}

bool IslandShelfScrollHit(IslandShelfScrollBox box, float x, float y) {
    if (!box.needed) return false;
    // The whole track, and a little wider than it looks: a 5 DIP bar is hard to
    // press exactly, and a miss reads as "the scrollbar does nothing".
    return x >= box.x - 6 && x < box.x + box.width + 6 && y >= box.top &&
           y < box.top + box.height;
}

bool IslandShelfScrollThumbHit(IslandShelfScrollBox box, float x, float y) {
    if (!box.needed) return false;
    return x >= box.x - 6 && x < box.x + box.width + 6 && y >= box.thumbTop &&
           y < box.thumbTop + box.thumbHeight;
}

int IslandShelfScrollOffsetFor(IslandShelfScrollBox box, float y, int total, int visible) {
    if (!box.needed || visible <= 0) return 0;
    const float span = std::max(1.f, box.height - box.thumbHeight);
    // The pointer holds the thumb where it was grabbed, so the list follows the
    // hand rather than jumping to put the row under the cursor.
    const float at = std::clamp((y - box.top - box.thumbHeight / 2) / span, 0.f, 1.f);
    const int maxOffset = std::max(0, total - visible);
    return static_cast<int>(std::lround(at * static_cast<float>(maxOffset)));
}

int IslandShelfArrowAt(float x, float y, float width, int totalPages, int currentPage) {
    if (totalPages <= 1) return 0;
    // Vertically centred on the chip area, so the arrows point at what they page.
    const float cy = IslandShelfFileTop + IslandShelfFileRowHeight * IslandShelfFileRows / 2;
    if (std::abs(y - cy) > IslandShelfArrowRadius + 4) return 0;
    // The same x the drawing uses, so the button and its tap area agree.
    if (currentPage > 0 && std::abs(x - 12.f) <= IslandShelfArrowRadius + 4) return -1;
    if (currentPage + 1 < totalPages && std::abs(x - (width - 12.f)) <= IslandShelfArrowRadius + 4)
        return 1;
    return 0;
}

void DrawIslandShelfPage(WidgetCanvas& c, IslandShelfTab active,
                         const std::vector<IslandClipRow>& clips, const std::wstring& clipsEmpty,
                         const std::vector<IslandFileRow>& files, const std::wstring& filesEmpty,
                         int copiedRow, int clipOffset, int filePage,
                         float w, float /*h*/, float alpha) {
    if (alpha <= .01f) return;
    const float right = w - kMargin;
    const bool clipboard = active == IslandShelfTab::Clipboard;
    const bool hasRows = clipboard ? !clips.empty() : !files.empty();

    // ---- header: title on the left, "clear" on the right.
    c.Text(Header(), {kMargin, IslandShelfHeaderY, 130, 18}, 12, 0xffffff, .95f * alpha,
           WidgetTextWeight::Semibold);
    if (hasRows) {
        c.Text(ClearLabel(), {right - 40, IslandShelfHeaderY + 2, 40, 16}, 10, 0xffffff,
               .62f * alpha, WidgetTextWeight::Medium, WidgetTextAlign::Right);
    }

    // ---- tab strip: the active pill is filled, the other is dimmed.
    for (int i = 0; i < 2; ++i) {
        const auto tab = static_cast<IslandShelfTab>(i);
        const bool current = tab == active;
        const float left = kMargin + (i == 0 ? 0.f : 58.f + 8.f);
        const auto label = tab == IslandShelfTab::Clipboard ? ClipboardLabel() : FilesLabel();
        if (current)
            c.Rounded({left, IslandShelfTabTop, 58, kTabHeight}, 10, 0x2f6bff, .92f * alpha);
        c.Text(label, {left, IslandShelfTabTop + 4, 58, 14}, 11, 0xffffff,
               (current ? .98f : .55f) * alpha, WidgetTextWeight::Medium, WidgetTextAlign::Center);
    }

    if (!hasRows) {
        c.Text(FitWeightedText(c, clipboard ? clipsEmpty : filesEmpty, w - kMargin * 2, 11,
                               WidgetTextWeight::Normal),
               {kMargin, 92, w - kMargin * 2, 18}, 11, 0xffffff, .48f * alpha);
        return;
    }

    if (clipboard) {
        // ---- clipboard rows: dot, text, time, pin, copy, remove. The row that
        // was just re-copied is tinted and says so, so a tap has visible proof.
        const int total = static_cast<int>(clips.size());
        const int first = std::clamp(clipOffset, 0, std::max(0, total - 1));
        for (int slot = 0; slot < IslandShelfClipRows; ++slot) {
            const int index = first + slot;
            if (index >= total) break;
            const auto& row = clips[static_cast<std::size_t>(index)];
            const float top = IslandShelfClipTop + static_cast<float>(slot) * IslandShelfClipRowHeight;
            const bool justCopied = copiedRow == index;
            if (justCopied)
                c.Rounded({kMargin - 6, top, w - (kMargin - 6) * 2, IslandShelfClipRowHeight - 6}, 8,
                          0xffffff, .08f * alpha);
            c.Ellipse({kMargin - 1, top + 8, 4, 4}, 0xffffff, .35f * alpha);

            const float textLeft = kMargin + 10;
            const float removeLeft = w - kMargin - kRemoveWidth -
                (IslandShelfScrollBar(w, total, IslandShelfClipRows, first).needed
                    ? IslandShelfScrollWidth + 6 : 0);
            const float copyLeft = removeLeft - kControlGap - kCopyWidth;
            const float pinLeft = copyLeft - kControlGap - kPinWidth;
            const float stampRight = pinLeft - 6;
            const float stampWidth = row.when.empty() ? 0.f : 30.f;
            const float lineWidth = stampRight - stampWidth - textLeft - 4;
            if (lineWidth > 24) {
                c.Text(FitWeightedText(c, row.line, lineWidth, 11, WidgetTextWeight::Normal),
                       {textLeft, top + 3, lineWidth, 16}, 11, 0xffffff, .90f * alpha);
            }
            if (!row.when.empty())
                c.Text(row.when, {stampRight - stampWidth, top + 5, stampWidth, 14}, 9, 0xffffff,
                       .36f * alpha, WidgetTextWeight::Normal, WidgetTextAlign::Right);
            Pin(c, pinLeft + kPinWidth / 2, top + 9, row.pinned, alpha);
            // The copy button turns into the confirmation, so the feedback is
            // where the finger already is rather than somewhere else on screen.
            if (justCopied)
                c.Text(CopiedLabel(), {copyLeft - 6, top + 5, kCopyWidth + 12, 14}, 9, 0x7ee2b8,
                       .98f * alpha, WidgetTextWeight::Medium, WidgetTextAlign::Center);
            else if (row.canCopy)
                c.Text(L"\u590d\u5236", {copyLeft, top + 5, kCopyWidth, 14}, 9, 0xffffff,
                       .66f * alpha, WidgetTextWeight::Medium, WidgetTextAlign::Center);
            Cross(c, removeLeft + kRemoveWidth / 2, top + 9, 4.5f, 0xffffff, .5f * alpha);
        }
        // A real scrollbar: the track is faint, the thumb is what says there is
        // more below and where the list stands.
        const auto bar = IslandShelfScrollBar(w, total, IslandShelfClipRows, first);
        if (bar.needed) {
            c.Rounded({bar.x, bar.top, bar.width, bar.height}, bar.width / 2, 0xffffff, .10f * alpha);
            c.Rounded({bar.x, bar.thumbTop, bar.width, bar.thumbHeight}, bar.width / 2, 0xffffff,
                      .45f * alpha);
        }
        return;
    }

    // ---- file chips: icon, name, kind; the × sits on the icon's corner.
    const int perPage = IslandShelfFileColumns * IslandShelfFileRows;
    const int pageCount = IslandShelfFilePages(static_cast<int>(files.size()));
    const int page = std::clamp(filePage, 0, std::max(0, pageCount - 1));
    const float cellW = IslandShelfFileCellWidth(w);
    const int firstFile = page * perPage;
    const int lastFile = std::min<int>(static_cast<int>(files.size()), firstFile + perPage);
    // Arrows appear only when they lead somewhere, drawn as round buttons at the
    // content edges rather than the reference's outside-the-frame chevrons.
    const float arrowCy = IslandShelfFileTop + IslandShelfFileRowHeight * IslandShelfFileRows / 2;
    const auto arrow = [&](bool previous, bool enabled) {
        // Just inside the content edges, clear of the chips: a button half over
        // the capsule's rounded corner is not pressable.
        const float cx = previous ? 12.f : w - 12.f;
        const auto tint = enabled ? .85f : .22f;
        c.Ellipse({cx - IslandShelfArrowRadius, arrowCy - IslandShelfArrowRadius,
                   IslandShelfArrowRadius * 2, IslandShelfArrowRadius * 2}, 0xffffff, .10f * alpha);
        const float d = previous ? -1.f : 1.f;
        c.Line(cx + 2 * d, arrowCy - 4, cx - 2 * d, arrowCy, 0xffffff, tint * alpha, 1.8f);
        c.Line(cx - 2 * d, arrowCy, cx + 2 * d, arrowCy + 4, 0xffffff, tint * alpha, 1.8f);
    };
    if (page > 0) arrow(true, true);
    if (page + 1 < pageCount) arrow(false, true);

    for (int i = firstFile; i < lastFile; ++i) {
        const auto& file = files[static_cast<std::size_t>(i)];
        const int slotIndex = i - firstFile;
        const int column = slotIndex % IslandShelfFileColumns;
        const int row = slotIndex / IslandShelfFileColumns;
        const float left = IslandShelfFileArrowGutter + static_cast<float>(column) * cellW;
        const float top = IslandShelfFileTop + static_cast<float>(row) * IslandShelfFileRowHeight;
        const float iconSize = 32;
        const float iconTop = top + 2;
        const float iconLeft = left + (cellW - iconSize) / 2;
        if (file.icon && file.icon->width) {
            c.ImageBitmap(file.icon, {iconLeft, iconTop, iconSize, iconSize}, alpha);
        } else {
            // No icon extracted: a plain sheet with the kind under it is still
            // recognisable, which beats an empty slot.
            c.Rounded({iconLeft + 4, iconTop, iconSize - 8, iconSize}, 3, 0xffffff, .16f * alpha);
        }
        // The chip's name, truncated by the cell rather than by the page.
        c.Text(FitWeightedText(c, file.name, cellW - 10, 10, WidgetTextWeight::Medium),
               {left + 5, iconTop + iconSize + 2, cellW - 10, 14}, 10, 0xffffff,
               .88f * alpha, WidgetTextWeight::Medium, WidgetTextAlign::Center);
        if (!file.kind.empty())
            c.Text(file.kind, {left + 5, iconTop + iconSize + 17, cellW - 10, 12}, 9,
                   0xffffff, .40f * alpha, WidgetTextWeight::Normal, WidgetTextAlign::Center);
        c.Ellipse({iconLeft + iconSize - 9, iconTop - 5, 14, 14}, 0x1b1b1f, .95f * alpha);
        Cross(c, iconLeft + iconSize - 2, iconTop + 2, 3.5f, 0xffffff, .85f * alpha);
    }
}

int IslandShelfFilePages(int count) {
    const int perPage = IslandShelfFileColumns * IslandShelfFileRows;
    if (count <= 0 || perPage <= 0) return 0;
    return (count + perPage - 1) / perPage;
}

}  // namespace lume::platform::windows
