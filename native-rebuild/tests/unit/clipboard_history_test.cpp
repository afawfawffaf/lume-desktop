// The clipboard shelf, checked without touching the real clipboard: the rules
// that decide what is kept, how the list bounds itself, and that a restart finds
// the same entries again. The capture itself is a thin call over the Win32 API;
// everything that can go subtly wrong is in the list below it.
#include "platform/windows/clipboard/clipboard_history.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace lume::platform::windows::clipboard;

namespace {
void Expect(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
}  // namespace

int main() {
    {
        // The rolling limit keeps the newest thirty and drops the oldest.
        History history;
        for (int i = 0; i < 40; ++i)
            Expect(history.Add("item-" + std::to_string(i)).has_value(), "添加失败");
        const auto rows = history.Rows();
        Expect(rows.size() == 30, "未保留最近 30 条");
        // Newest first: the last thing copied is the row the user sees on top.
        Expect(rows.front().text == "item-39", "最新一条不在首位");
        Expect(rows.back().text == "item-10", "最旧一条应被挤出");
    }
    {
        // Copying the same text twice in a row is one row, not two.
        History history;
        Expect(history.Add("hello").has_value(), "首次添加失败");
        Expect(!history.Add("hello").has_value(), "重复内容应被忽略");
        Expect(history.Size() == 1, "重复内容产生了第二行");
        // A different text between them makes the repeat a new entry again.
        history.Add("other");
        Expect(history.Add("hello").has_value(), "间隔后的重复应被记录");
        Expect(history.Size() == 3, "重复内容数量不对");
    }
    {
        // A pinned row survives the rolling limit, because it is the user's own
        // choice and newer copies must not push it out.
        History history;
        const auto pinned = history.Add("keep me");
        Expect(pinned.has_value(), "添加置顶项失败");
        Expect(history.TogglePin(pinned->id), "置顶失败");
        for (int i = 0; i < 40; ++i) history.Add("filler-" + std::to_string(i));
        const auto rows = history.Rows();
        bool found = false;
        for (const auto& row : rows) if (row.text == "keep me") found = true;
        Expect(found, "置顶项被新内容挤出");
        // Unpinned rows still obey the limit around it.
        Expect(rows.size() <= 31, "置顶后数量超出上限");
    }
    {
        // Oversized and empty text is refused: a pasted log would otherwise
        // dominate the file and the page.
        History history;
        Expect(!history.Add("").has_value(), "空内容应被拒绝");
        Expect(!history.Add(std::string(64 * 1024, 'x')).has_value(), "超大内容应被拒绝");
        Expect(history.Size() == 0, "被拒绝的内容却入了库");
    }
    {
        // Pinning, removing and clearing address one row by its id.
        History history;
        const auto a = history.Add("a");
        const auto b = history.Add("b");
        Expect(a.has_value() && b.has_value(), "添加失败");
        Expect(history.TogglePin(a->id), "置顶失败");
        Expect(history.Remove(b->id), "删除失败");
        Expect(!history.Remove(b->id), "重复删除应报告未命中");
        Expect(history.Size() == 1, "删除后数量不对");
        // Clearing keeps the pinned rows: they were deliberate.
        history.Add("c");
        history.Clear();
        const auto left = history.Rows();
        Expect(left.size() == 1 && left[0].text == "a", "清空应保留置顶项");
    }
    {
        // A restart finds the same entries, pinned state included.
        const auto path = std::filesystem::temp_directory_path() / "lume-clipboard-test.txt";
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        {
            History history;
            const auto kept = history.Add("line one\nline two");
            history.Add("plain");
            if (kept) history.TogglePin(kept->id);
            Expect(history.Save(path), "保存失败");
        }
        {
            History history;
            Expect(history.Load(path), "读取失败");
            const auto rows = history.Rows();
            Expect(rows.size() == 2, "重启后条目数不对");
            // The newline has to survive the round trip through one line.
            Expect(rows[1].text == "line one\nline two", "多行内容未正确保存");
            Expect(rows[1].pinned, "置顶状态丢失");
        }
        std::filesystem::remove(path, ignored);
    }
    {
        // A missing or damaged file is an empty history, never an exception: a
        // half-written file after a crash must not stop the application.
        History history;
        Expect(!history.Load(std::filesystem::temp_directory_path() / "lume-no-such-file.txt"),
               "不存在的文件应返回 false");
        const auto broken = std::filesystem::temp_directory_path() / "lume-clipboard-broken.txt";
        { std::ofstream out(broken); out << "not the header\nwhatever\n"; }
        Expect(!history.Load(broken), "格式错误的文件应被拒绝");
        Expect(history.Size() == 0, "损坏文件不应产生条目");
        std::error_code ignored;
        std::filesystem::remove(broken, ignored);
    }
    {
        // The island draws one line: newlines and runs of spaces collapse, and a
        // long snippet is cut rather than running past the capsule.
        Expect(ToLine("hello world", 40) == L"hello world", "普通文本转换错误");
        Expect(ToLine("a\nb", 40) == L"a b", "换行应折叠成空格");
        Expect(ToLine("a   b", 40) == L"a b", "连续空格应折叠");
        Expect(ToLine("\n\nlead", 40) == L"lead", "开头空白应去掉");
        const auto cut = ToLine(std::string(100, 'x'), 10);
        Expect(cut.size() <= 11, "超长文本未截断");
        Expect(cut.back() == static_cast<wchar_t>(0x2026), "截断应带省略号");
        Expect(ToLine("", 40).empty(), "空文本应得到空行");
    }
    std::cout << "clipboard: history, persistence and line rules passed\n";
    return 0;
}
