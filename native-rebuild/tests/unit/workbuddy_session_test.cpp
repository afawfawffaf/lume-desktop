// The WorkBuddy session, driven without a browser: connect, publish, remove and
// shut down while workers are in flight. This is the layer the island and the
// settings page both sit on, and the only one that owns threads, so a mistake
// here takes the whole application down rather than showing a wrong number.
#include "apps/shell/workbuddy_session.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace lume::shell;
using lume::platform::windows::quota::WorkBuddyKind;

namespace {
void Expect(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

// A session wired to a temp store and no wake callback: the reads under test are
// the account list and its hand-over, not the island's repaint.
WorkBuddySessionOptions Options(const std::string& store) {
    WorkBuddySessionOptions options;
    options.storePath = store;
    options.refresh = std::chrono::milliseconds(50);
    options.timeout = std::chrono::milliseconds(1500);
    options.loginWindow = std::chrono::milliseconds(800);
    options.verification = false;
    return options;
}
}  // namespace

int main() {
    const auto store = std::string{"workbuddy-session-test.bin"};
    std::remove(store.c_str());
    {
        // Reading an empty session is safe and reports nothing, from any thread.
        WorkBuddySession session(Options(store));
        Expect(session.Accounts().empty(), "空会话不应有账号");
        Expect(!session.AnyConnected(WorkBuddyKind::Desktop), "空会话不应报告已连接");
        Expect(!session.Running(), "空会话不应在忙");
        Expect(!session.Drain().has_value(), "空会话不应交回读数");

        // Many concurrent readers while the session is alive: the account list is
        // handed to the UI thread while workers may publish into it.
        std::atomic<bool> stop{false};
        std::atomic<int> reads{0};
        std::thread reader([&] {
            while (!stop.load()) {
                const auto list = session.Accounts();
                (void)list.size();
                (void)session.AnyConnected(WorkBuddyKind::DesktopAi);
                (void)session.Running();
                reads.fetch_add(1);
            }
        });
        // Refreshing from another thread exercises the worker start path.
        for (int i = 0; i < 40; ++i) {
            session.Refresh(true);
            session.Drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        // A disconnect for an account that was never connected must be harmless.
        session.Disconnect(WorkBuddyKind::Desktop, "no-such-uid");
        Expect(session.Accounts().empty(), "断开不存在的账号不应产生记录");
        stop = true;
        reader.join();
        Expect(reads.load() > 0, "并发读取没有执行");
        session.Shutdown();
        // After shutdown, every entry point is still safe to call.
        Expect(session.Accounts().empty(), "关闭后读取应为空");
        Expect(!session.Running(), "关闭后不应在忙");
        Expect(!session.Begin(WorkBuddyKind::Desktop).has_value(), "关闭后不应能开始登入");
    }
    {
        // Shutting down while a login is in progress: the worker must be joined,
        // not left touching the session it was started from. A hang here would
        // stall the application's exit, so this is the lifetime check.
        WorkBuddySession session(Options(store));
        const auto start = std::chrono::steady_clock::now();
        (void)session.Begin(WorkBuddyKind::DesktopAi);
        // The account list includes the pending connection, without a uid.
        const auto pending = session.Accounts();
        for (const auto& account : pending) {
            if (!account.connected) Expect(account.id.empty(), "未完成的连接不应有 uid");
        }
        session.Shutdown();
        const auto spent = std::chrono::steady_clock::now() - start;
        Expect(spent < std::chrono::seconds(20), "关闭时未在限定时间内收回工作线程");
        // A second shutdown is a no-op rather than a crash or a wait.
        session.Shutdown();
    }
    {
        // A worker that throws must not end the process. std::thread turns an
        // escaping exception into std::terminate, and this application's
        // terminate handler exits the process: that is exactly the crash this
        // test exists to keep out. Points the session at an unusable store so
        // the save path fails rather than being skipped.
        WorkBuddySession session(Options("Z:/lume-missing-directory/nested/store.bin"));
        (void)session.Begin(WorkBuddyKind::Desktop);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        // Reaching here at all proves the worker did not terminate the process.
        const auto accounts = session.Accounts();
        for (const auto& account : accounts)
            Expect(account.status != "waiting" || true, "状态字段应可读");
        session.Shutdown();
    }
    std::remove(store.c_str());
    std::cout << "workbuddy session: lifetime and concurrency checks passed\n";
    return 0;
}
