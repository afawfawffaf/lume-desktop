#pragma once

#include <windows.h>
#include <objidl.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lume::platform::windows {

// What the island wants to know about a drag hovering over it. `count` is how
// many items the drag carries, so the page can say "3 个文件" before the drop.
struct IslandDropHover {
    bool over = false;
    POINT point{};  // client-area physical pixels
    std::size_t count = 0;
};

// A drop target for the island, so a file can be dragged onto it and kept.
// RegisterDragDrop needs an OLE-initialized thread and one target per HWND; the
// island owns exactly one. Only file paths are accepted: a drag of text or an
// image is refused, which is what keeps the shelf's meaning unambiguous.
class IslandDropTarget final {
public:
    using Hover = std::function<void(const IslandDropHover&)>;
    using OnDrop = std::function<void(std::vector<std::string>, POINT)>;

    IslandDropTarget();
    ~IslandDropTarget();
    IslandDropTarget(const IslandDropTarget&) = delete;
    IslandDropTarget& operator=(const IslandDropTarget&) = delete;

    // Attach, Detach and the destructor must run on the window's thread; the
    // callbacks run there too. Paths are UTF-8. Nothing is copied or moved here.
    bool Attach(HWND, Hover, OnDrop);
    void Detach();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lume::platform::windows
