// The WorkBuddy store, checked without a browser or a network call: what it keeps
// across a restart, that one account's removal leaves the others alone, and that
// a record written before accounts were keyed by uid is still readable. That last
// case is the one that matters: the user cannot be asked to authorise again just
// because the file's shape changed.
#include "platform/windows/quota/workbuddy_store.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

using namespace lume::platform::windows::quota;

namespace {
void Expect(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

// A token whose payload carries `sub`, base64url encoded exactly as the API
// issues it. Only the middle segment is ever read, so the signature is filler.
std::string TokenFor(const std::string& sub) {
    const auto payload = "{\"sub\":\"" + sub + "\",\"nickname\":\"x\"}";
    DWORD needed = 0;
    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(payload.data()),
                         static_cast<DWORD>(payload.size()),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &needed);
    std::string base64(needed, '\0');
    CryptBinaryToStringA(reinterpret_cast<const BYTE*>(payload.data()),
                         static_cast<DWORD>(payload.size()),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, base64.data(), &needed);
    base64.resize(needed);
    while (!base64.empty() && (base64.back() == '=' || base64.back() == '\r' || base64.back() == '\n'))
        base64.pop_back();
    for (auto& ch : base64) {
        if (ch == '+') ch = '-';
        else if (ch == '/') ch = '_';
    }
    return "header." + base64 + ".signature";
}

// Writes the pre-migration shape straight to disk: one object per slot under a
// build object, and no uid beside the token.
bool WriteLegacy(const std::filesystem::path& path, const std::string& token) {
    const auto plain = std::string{"{\"desktop\":{\"slot1\":{\"accessToken\":\""} + token +
        "\",\"refreshToken\":\"\",\"domain\":\"www.codebuddy.cn\",\"label\":\"\",\"uid\":\"\"}}}";
    auto lower = std::filesystem::absolute(path).lexically_normal().wstring();
    CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    std::string entropy = "Lume.Quota.WorkBuddy.v1|";
    entropy.append(reinterpret_cast<const char*>(lower.data()), lower.size() * sizeof(wchar_t));
    DATA_BLOB source{static_cast<DWORD>(plain.size()),
                     reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB extra{static_cast<DWORD>(entropy.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(entropy.data()))};
    DATA_BLOB blob{};
    if (!CryptProtectData(&source, L"Lume WorkBuddy credentials", &extra, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &blob))
        return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { LocalFree(blob.pbData); return false; }
    DWORD written = 0;
    const auto ok = WriteFile(file, blob.pbData, blob.cbData, &written, nullptr) && written == blob.cbData;
    CloseHandle(file);
    LocalFree(blob.pbData);
    return ok;
}

WorkBuddySession Session(const std::string& uid, const std::string& label) {
    WorkBuddySession session;
    session.accessToken = TokenFor(uid);
    session.uid = uid;
    session.domain = "www.codebuddy.cn";
    session.label = label;
    return session;
}
}  // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() / "lume-workbuddy-store-test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory, ignored);
    const auto path = directory / "workbuddy-accounts.bin";
    {
        WorkBuddyStore store(path);
        // A fresh store is empty, not an error.
        Expect(store.Load(WorkBuddyKind::Desktop).empty(), "空存储应读为空");
        Expect(!store.Any(WorkBuddyKind::Desktop), "空存储不应报告有账号");

        // The count is not fixed: three accounts under one build all survive.
        Expect(store.Save(WorkBuddyKind::Desktop, Session("uid-a", "甲")), "保存账号 A 失败");
        Expect(store.Save(WorkBuddyKind::Desktop, Session("uid-b", "乙")), "保存账号 B 失败");
        Expect(store.Save(WorkBuddyKind::DesktopAi, Session("uid-c", "丙")), "保存账号 C 失败");
        const auto desktop = store.Load(WorkBuddyKind::Desktop);
        Expect(desktop.size() == 2, "桌面版账号数应为 2");
        Expect(store.Load(WorkBuddyKind::DesktopAi).size() == 1, "AI 版账号数应为 1");
        // Insertion order is kept, so the page does not reshuffle between launches.
        Expect(desktop[0].uid == "uid-a" && desktop[1].uid == "uid-b", "账号顺序应保持插入顺序");
        Expect(desktop[0].label == "甲", "标签未保留");
        // The two builds are independent: saving one never touches the other.
        Expect(store.Any(WorkBuddyKind::DesktopAi), "AI 版应报告有账号");

        // Saving the same uid replaces it in place rather than adding a copy.
        Expect(store.Save(WorkBuddyKind::Desktop, Session("uid-a", "甲改")), "更新账号 A 失败");
        const auto updated = store.Load(WorkBuddyKind::Desktop);
        Expect(updated.size() == 2 && updated[0].label == "甲改", "重复保存应原地替换");

        // Removing one account leaves the others untouched.
        store.Clear(WorkBuddyKind::Desktop, "uid-a");
        const auto left = store.Load(WorkBuddyKind::Desktop);
        Expect(left.size() == 1 && left[0].uid == "uid-b", "删除一个账号不应影响其他账号");
        Expect(store.Load(WorkBuddyKind::DesktopAi).size() == 1, "删除不应跨越版本");
        // Removing an absent uid is not an error.
        store.Clear(WorkBuddyKind::Desktop, "uid-missing");
        Expect(store.Load(WorkBuddyKind::Desktop).size() == 1, "删除不存在的账号不应改动列表");
    }
    {
        // A record written before accounts were keyed by uid must still load:
        // the uid is recovered from the token's own subject.
        WorkBuddyStore store(path);
        Expect(!store.Load(WorkBuddyKind::DesktopAi).empty(), "AI 版账号应在重建后保留");
        std::filesystem::remove(path, ignored);
        Expect(WriteLegacy(path, TokenFor("legacy-uid")), "写入旧格式失败");
        const auto migrated = store.Load(WorkBuddyKind::Desktop);
        Expect(migrated.size() == 1, "旧格式的账号应被读出，而不是丢弃");
        Expect(migrated[0].uid == "legacy-uid", "旧格式账号的 uid 应从令牌主题恢复");
        // A save carries the older record forward into the list form.
        Expect(store.Save(WorkBuddyKind::Desktop, Session("new-uid", "新")), "迁移后保存失败");
        const auto after = store.Load(WorkBuddyKind::Desktop);
        Expect(after.size() == 2, "旧账号应与新账号同时存在");
        Expect(after[0].uid == "legacy-uid" && after[1].uid == "new-uid", "旧账号应排在新账号之前");
    }
    {
        // An unreadable file is an empty store, never a crash and never a
        // half-written list.
        const auto broken = directory / "broken.bin";
        HANDLE file = CreateFileW(broken.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const char junk[] = "not a dpapi blob";
            DWORD written = 0;
            WriteFile(file, junk, sizeof(junk) - 1, &written, nullptr);
            CloseHandle(file);
        }
        WorkBuddyStore store(broken);
        Expect(store.Load(WorkBuddyKind::Desktop).empty(), "损坏文件应读为空而不是崩溃");
        // Saving over it starts a fresh file rather than failing forever.
        Expect(store.Save(WorkBuddyKind::Desktop, Session("uid-z", "赵")), "损坏文件上应能重建");
        Expect(store.Load(WorkBuddyKind::Desktop).size() == 1, "重建后应读出一个账号");
    }

    {
        // The real file this machine already has, if present: an account
        // authorised before the uid was stored must still load. This is the case
        // that lost the user's account, so it is checked against the actual file
        // rather than a reconstruction of it.
        const auto real = std::filesystem::path{L"C:\\Users\\liuwang\\AppData\\Local\\Lume\\workbuddy-accounts.bin"};
        std::error_code present;
        if (std::filesystem::exists(real, present)) {
            WorkBuddyStore actual(real);
            const auto accounts = actual.Load(WorkBuddyKind::Desktop);
            for (const auto& account : accounts)
                std::cout << "real store: uid=" << account.uid
                          << " tokenLen=" << account.accessToken.size() << "\n";
            Expect(accounts.empty() || !accounts[0].uid.empty(),
                   "真实存储里的账号必须能恢复出 uid");
        }
    }
    {
        // What a completed sign-in does, in order: save the account, then read
        // the list the page shows. The account count grows each time, which is
        // the case that used to reallocate the row list mid-read.
        const auto flow = directory / "flow.bin";
        WorkBuddyStore store(flow);
        for (int i = 0; i < 6; ++i)
            Expect(store.Save(WorkBuddyKind::DesktopAi, Session("flow-" + std::to_string(i), "账号")),
                   "连续登入保存失败");
        const auto all = store.Load(WorkBuddyKind::DesktopAi);
        Expect(all.size() == 6, "连续登入的账号应全部保留");
        for (int i = 0; i < 6; ++i)
            Expect(all[static_cast<std::size_t>(i)].uid == "flow-" + std::to_string(i),
                   "连续登入后顺序应保持");
        // Removing from the middle keeps the rest intact.
        store.Clear(WorkBuddyKind::DesktopAi, "flow-3");
        const auto after = store.Load(WorkBuddyKind::DesktopAi);
        Expect(after.size() == 5, "删除中间账号后数量应为 5");
        Expect(after[2].uid == "flow-2" && after[3].uid == "flow-4", "删除中间账号不应打乱其余顺序");
    }
    std::filesystem::remove_all(directory, ignored);
    std::cout << "workbuddy store: migration and multi-account checks passed\n";
    return 0;
}
