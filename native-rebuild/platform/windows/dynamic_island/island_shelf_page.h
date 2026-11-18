#pragma once

#include "platform/windows/widget_ui/widget_canvas.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lume::platform::windows {

// One clipboard row as the page draws it. The page is handed strings and flags
// only; it never touches the clipboard, so drawing cannot block on it.
struct IslandClipRow {
    std::uint64_t id = 0;
    std::wstring line;   // already flattened to one line by the caller
    std::wstring when;   // "09:34"-style stamp, or empty
    bool pinned = false;
    bool canCopy = false;
};

// One file chip. The icon is pixels the host already extracted: the page never
// reads the filesystem, and a missing icon simply draws the type label instead.
struct IslandFileRow {
    std::uint64_t id = 0;
    std::wstring name;      // file name only
    std::wstring kind;      // "PDF", "DOC", "PNG", ... shown under the icon
    std::shared_ptr<const WidgetBitmap> icon;
};

// The two tabs of the shelf: recent copies, and the files dropped on the island.
enum class IslandShelfTab { Clipboard, Files };

// What a tap on a shelf row asks the host to do. The window never edits the
// history or the files itself, so every one of these is a request.
// DragOut is not a click action: it is reported when a chip is dragged away, so
// the host can hand the file to the shell's own drag machinery.
enum class IslandShelfAction { Copy, Pin, Remove, OpenFile, RemoveFile, ClearAll };

// The layout is fixed so the drawing and the hit tests cannot disagree.
inline constexpr float IslandShelfHeaderY = 10;
inline constexpr float IslandShelfTabTop = 34;
// How many clipboard rows are visible at once. The list scrolls: the reference
// shows four and keeps the rest behind a scrollbar, which is what this does.
inline constexpr int IslandShelfClipRows = 4;
inline constexpr float IslandShelfClipRowHeight = 30;
inline constexpr float IslandShelfClipTop = 64;
// Files: four per row, as many rows as fit. The reference puts five across, but
// the island's capsule is narrower, so four keeps the names readable.
inline constexpr int IslandShelfFileColumns = 4;
// The grid is measured, not fixed: four chips plus the two arrow gutters have to
// fit whatever width the capsule has, and a hard-coded cell width overflowed it.
inline constexpr float IslandShelfFileArrowGutter = 26;
inline constexpr float IslandShelfFileTop = 64;
inline constexpr float IslandShelfFileRowHeight = 68;
inline constexpr int IslandShelfFileRows = 2;

// Which tab a point selects, or -1.
int IslandShelfTabAt(float x, float y, float width);
// The clipboard row a point selects, or -1.
int IslandShelfClipRowAt(float x, float y, float width, int rows);
// Which of its buttons a point on a clipboard row selects: 0 = the pin,
// 1 = the re-copy button, 2 = the remove button, -1 = the row body.
int IslandShelfClipButtonAt(float x, float y, float width, int rows);
// The file cell a point selects, or -1, and which of its buttons: 0 = remove.
// The chip width for a page `width` wide: the content between the two gutters
// split by the column count, so the grid never overruns the arrows.
float IslandShelfFileCellWidth(float width);
int IslandShelfFileCellAt(float x, float y, float width, int cells);
int IslandShelfFileButtonAt(float x, float y, float width, int cells);
// The "clear all" control at the top right, when the tab has entries.
bool IslandShelfClearAt(float x, float y, float width);

// The clipboard's scrollbar. It is a real control, not a decoration: pressing it
// and dragging moves the list, which is how the reference behaves and the only
// way to reach rows past the fourth.
inline constexpr float IslandShelfScrollWidth = 5;
inline constexpr float IslandShelfScrollLeft = 4;  // from the page's right edge
struct IslandShelfScrollBox {
    float x = 0, top = 0, width = 0, height = 0;  // the track
    float thumbTop = 0, thumbHeight = 0;          // the visible thumb
    bool needed = false;
};
// The track and thumb for `total` rows showing `visible` at `offset`.
IslandShelfScrollBox IslandShelfScrollBar(float width, int total, int visible, int offset);
// True when a point is anywhere on the scrollbar's track. A press on the thumb
// starts a drag; a press elsewhere on the track jumps there, which is the
// behaviour a scrollbar is expected to have.
bool IslandShelfScrollHit(IslandShelfScrollBox box, float x, float y);
// True when a point is on the thumb itself.
bool IslandShelfScrollThumbHit(IslandShelfScrollBox box, float x, float y);
// The row offset a thumb dragged to `y` means.
int IslandShelfScrollOffsetFor(IslandShelfScrollBox box, float y, int total, int visible);

// The files' paging arrows, one on each side, shown when there is another page.
// They are round buttons the width of the margin would not fit, so they sit just
// inside the content edges.
inline constexpr float IslandShelfArrowRadius = 11;
// Which side an arrow tap selects: -1 previous, 1 next, 0 none.
int IslandShelfArrowAt(float x, float y, float width, int totalPages, int currentPage);

void DrawIslandShelfPage(WidgetCanvas&, IslandShelfTab active,
                         const std::vector<IslandClipRow>& clips, const std::wstring& clipsEmpty,
                         const std::vector<IslandFileRow>& files, const std::wstring& filesEmpty,
                         int copiedRow, int clipOffset, int filePage,
                         float width, float height, float alpha);
// How many pages the file grid needs for `count` chips.
int IslandShelfFilePages(int count);

}  // namespace lume::platform::windows
