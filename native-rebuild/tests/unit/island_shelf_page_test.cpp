// The shelf page's scrollbar and paging geometry, checked without a window: the
// numbers the drawing uses have to be the numbers a tap lands on, or the control
// looks present but does nothing.
#include "platform/windows/dynamic_island/island_shelf_page.h"
#include "platform/windows/dynamic_island/island_dots.h"

#include <cstdlib>
#include <iostream>

using namespace lume::platform::windows;

namespace {
void Expect(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
constexpr float kWidth = 404;  // the island's expanded width in DIP
}  // namespace

int main() {
    {
        // A list that fits needs no scrollbar, and one that does shows a thumb.
        // The bar is always present; a full-height thumb means "nothing to scroll".
        const auto none = IslandShelfScrollBar(kWidth, 3, IslandShelfClipRows, 0);
        Expect(none.needed, "滚动条应始终显示");
        Expect(none.thumbHeight >= none.height - 0.5f, "无需滚动时滑块应占满轨道");
        const auto bar = IslandShelfScrollBar(kWidth, 10, IslandShelfClipRows, 0);
        Expect(bar.needed, "超过可见行数时应有滚动条");
        Expect(bar.thumbHeight >= 18, "滑块应有可抓取的最小高度");
        Expect(bar.thumbTop >= bar.top - 0.5f, "滑块不应越过轨道顶部");
        Expect(bar.thumbTop + bar.thumbHeight <= bar.top + bar.height + 0.5f, "滑块不应越过轨道底部");
        // The bar sits inside the page, not past its edge.
        Expect(bar.x > 0 && bar.x + bar.width <= kWidth, "滚动条应在页面内");
    }
    {
        // The thumb tracks the offset: first row at the top, last at the bottom.
        const auto top = IslandShelfScrollBar(kWidth, 10, IslandShelfClipRows, 0);
        const auto bottom = IslandShelfScrollBar(kWidth, 10, IslandShelfClipRows, 6);
        Expect(bottom.thumbTop > top.thumbTop, "滚动到底时滑块应更低");
        const auto mid = IslandShelfScrollBar(kWidth, 10, IslandShelfClipRows, 3);
        Expect(mid.thumbTop > top.thumbTop && mid.thumbTop < bottom.thumbTop, "滑块应随偏移移动");
    }
    {
        // A press anywhere on the track is a hit: a 5 DIP bar that only answers
        // on its exact pixels reads as broken.
        const auto bar = IslandShelfScrollBar(kWidth, 10, IslandShelfClipRows, 0);
        const float centreX = bar.x + bar.width / 2;
        Expect(IslandShelfScrollHit(bar, centreX, bar.top + 1), "轨道起点应可点");
        Expect(IslandShelfScrollHit(bar, centreX, bar.top + bar.height - 1), "轨道终点应可点");
        Expect(!IslandShelfScrollHit(bar, centreX, bar.top - 10), "轨道外不应命中");
        Expect(!IslandShelfScrollHit(bar, 10, bar.top + 1), "页面左侧不应命中");
        // The thumb is a sub-range of the track, which is what decides drag vs jump.
        Expect(IslandShelfScrollThumbHit(bar, centreX, bar.thumbTop + 2), "滑块自身应命中滑块");
        Expect(!IslandShelfScrollThumbHit(bar, centreX, bar.top + bar.height - 1) ||
               bar.thumbTop + bar.thumbHeight >= bar.top + bar.height - 1,
               "轨道底部若不在滑块上则不应判为滑块");
    }
    {
        // Mapping a pointer position to an offset is monotonic and clamped.
        const auto bar = IslandShelfScrollBar(kWidth, 10, IslandShelfClipRows, 0);
        const int first = IslandShelfScrollOffsetFor(bar, bar.top, 10, IslandShelfClipRows);
        const int last = IslandShelfScrollOffsetFor(bar, bar.top + bar.height, 10, IslandShelfClipRows);
        Expect(first == 0, "轨道顶部应对应第一行");
        Expect(last == 6, "轨道底部应对应最后一行");
        const int a = IslandShelfScrollOffsetFor(bar, bar.top + bar.height * .4f, 10, IslandShelfClipRows);
        const int b = IslandShelfScrollOffsetFor(bar, bar.top + bar.height * .6f, 10, IslandShelfClipRows);
        Expect(a <= b, "偏移应随指针下移而增加");
        Expect(a >= 0 && b <= 6, "偏移应被限制在范围内");
    }
    {
        // Paging: eight chips per page, and the arrows only exist where they lead.
        Expect(IslandShelfFilePages(0) == 0, "没有文件时不应有页");
        Expect(IslandShelfFilePages(4) == 1, "一格应为一页");
        Expect(IslandShelfFilePages(8) == 1, "满一页仍应为一页");
        Expect(IslandShelfFilePages(9) == 2, "超过一页应有两页");
        const float cy = IslandShelfFileTop + IslandShelfFileRowHeight * IslandShelfFileRows / 2;
        // First page: no previous arrow, a next arrow.
        Expect(IslandShelfArrowAt(12, cy, kWidth, 2, 0) == 0, "首页不应有上一页箭头");
        Expect(IslandShelfArrowAt(kWidth - 12, cy, kWidth, 2, 0) == 1, "首页应有下一页箭头");
        // Last page: the mirror image.
        Expect(IslandShelfArrowAt(12, cy, kWidth, 2, 1) == -1, "末页应有上一页箭头");
        Expect(IslandShelfArrowAt(kWidth - 12, cy, kWidth, 2, 1) == 0, "末页不应有下一页箭头");
        // A single page has no arrows at all.
        Expect(IslandShelfArrowAt(12, cy, kWidth, 1, 0) == 0, "单页不应有箭头");
    }
    {
        // The cells are measured from the width, so four chips plus the arrow
        // gutters can never overrun the page.
        const float cell = IslandShelfFileCellWidth(kWidth);
        const float span = IslandShelfFileArrowGutter * 2 + cell * IslandShelfFileColumns;
        Expect(span <= kWidth + 0.5f, "文件网格不应超出页面宽度");
        Expect(cell >= 56, "单元格不应窄到放不下文件名");
        // A narrow page still fits: the cell shrinks rather than overflowing.
        const float narrow = IslandShelfFileCellWidth(300);
        Expect(IslandShelfFileArrowGutter * 2 + narrow * IslandShelfFileColumns <= 300 + 0.5f,
               "窄页面下网格仍不应溢出");
    }
    {
        // The tab strip and the clear control are reachable where they are drawn.
        Expect(IslandShelfTabAt(20, IslandShelfTabTop + 4, kWidth) == 0, "第一个页签应可点");
        Expect(IslandShelfTabAt(90, IslandShelfTabTop + 4, kWidth) == 1, "第二个页签应可点");
        Expect(IslandShelfTabAt(200, IslandShelfTabTop + 4, kWidth) == -1, "页签之间不应命中");
        Expect(IslandShelfClearAt(kWidth - 20, IslandShelfHeaderY + 6, kWidth), "清空按钮应可点");
        Expect(!IslandShelfClearAt(20, IslandShelfHeaderY + 6, kWidth), "标题处不应命中清空");
    }
    {
        // The page dots are a picker: every dot a tap can reach must be the dot
        // that is drawn there, and the selected dot's extra width must not push a
        // neighbour's hit box off it.
        constexpr float kPageWidth = 404, kPageHeight = 208;
        for (int count = 2; count <= 8; ++count) {
            for (int selected = 0; selected < count; ++selected) {
                DotBox boxes[32]{};
                DotBoxes(count, selected, kPageWidth, boxes);
                for (int i = 0; i < count; ++i) {
                    const float cx = boxes[i].left + boxes[i].width / 2;
                    const float cy = kPageHeight - DotStripBottom + DotStripHeight / 2;
                    const int hit = DotAt(count, selected, kPageWidth, kPageHeight, cx, cy);
                    Expect(hit == i, "圆点命中应落在自己身上");
                    // Dots must not overlap: one box's centre is not inside another.
                    for (int j = 0; j < count; ++j)
                        if (j != i)
                            Expect(cx < boxes[j].left || cx >= boxes[j].left + boxes[j].width,
                                   "圆点不应重叠");
                }
                // The strip stays inside the page.
                Expect(boxes[0].left >= 0, "圆点条不应越过左边缘");
                Expect(boxes[count - 1].left + boxes[count - 1].width <= kPageWidth,
                       "圆点条不应越过右边缘");
            }
        }
        // Off the strip, and with a single page, nothing is selected.
        Expect(DotAt(5, 0, kPageWidth, kPageHeight, 200, 40) == -1, "页面上方不应命中圆点");
        Expect(DotAt(1, 0, kPageWidth, kPageHeight, 200, kPageHeight - 7) == -1, "单页不应有圆点");
    }
    std::cout << "shelf: scrollbar, paging and hit geometry passed\n";
    return 0;
}
