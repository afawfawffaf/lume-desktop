// The WorkBuddy wire formats, checked without a network call: what the parser
// accepts, what it refuses, and that a login URL has to be the one we asked for.
#include "platform/windows/quota/workbuddy_balance.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace lume::platform::windows::quota;

namespace {
void Expect(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
}  // namespace

int main() {
    {
        // The documented summary: a plan name and the four credit groups.
        const auto body = R"({"code":0,"data":{"SubscriptionPackageName":"体验版","CreditGroups":[
            {"Kind":"plan_base","CycleRemainCapacity":2000},
            {"Kind":"plan_bonus","CycleRemainCapacity":"494.82"},
            {"Kind":"addon","CycleRemainCapacity":0},
            {"Kind":"benefit","CycleRemainCapacity":0}]}})";
        const auto credits = ParseWorkBuddyCredits(body, 200);
        Expect(credits.ok, "正常响应未被接受");
        Expect(std::abs(credits.remaining - 2494.82) < 0.001, "积分合计错误");
        Expect(credits.plan == "体验版", "套餐名未读到");
        Expect(credits.parts.size() == 4, "分组数量错误");
        Expect(credits.parts[0].label == "plan" && credits.parts[1].label == "bonus", "分组标签错误");
    }
    {
        // An older response with only Packages still produces a figure.
        const auto credits = ParseWorkBuddyCredits(
            R"({"code":0,"data":{"Packages":[{"PackageCode":"p1","CycleRemainCapacity":100},
                {"PackageCode":"p2","CycleRemainCapacity":50}]}})", 200);
        Expect(credits.ok && std::abs(credits.remaining - 150) < 0.001, "Packages 回退失败");
    }
    {
        // Failures stay distinguishable: an expired session is not a zero
        // balance, and neither is a malformed body.
        Expect(ParseWorkBuddyCredits("{}", 401).error == "expired", "401 未判为过期");
        Expect(ParseWorkBuddyCredits("{}", 403).error == "expired", "403 未判为过期");
        Expect(ParseWorkBuddyCredits("{}", 500).error == "network", "500 未判为网络失败");
        Expect(ParseWorkBuddyCredits(R"({"code":1})", 200).error == "parse", "非零 code 未拒绝");
        Expect(ParseWorkBuddyCredits("not json", 200).error == "parse", "非 JSON 未拒绝");
        Expect(ParseWorkBuddyCredits(R"({"code":0,"data":{}})", 200).error == "parse", "缺分组未拒绝");
        Expect(ParseWorkBuddyCredits(R"({"code":0,"data":{"CreditGroups":[]}})", 200).error == "parse",
            "空分组未拒绝");
        Expect(ParseWorkBuddyCredits(
            R"({"code":0,"data":{"CreditGroups":[{"Kind":"unknown","CycleRemainCapacity":5}]}})", 200).error == "parse",
            "未知分组类型未拒绝");
        Expect(ParseWorkBuddyCredits(
            R"({"code":0,"data":{"CreditGroups":[{"Kind":"plan_base","CycleRemainCapacity":"abc"}]}})", 200).error == "parse",
            "非数字金额未拒绝");
        // A zero balance is a valid reading, not a failure.
        const auto zero = ParseWorkBuddyCredits(
            R"({"code":0,"data":{"CreditGroups":[{"Kind":"plan_base","CycleRemainCapacity":0}]}})", 200);
        Expect(zero.ok && zero.remaining == 0, "零余额应判为有效读数");
    }
    {
        // An empty or unprintable token never reaches the network.
        Expect(FetchWorkBuddyCredits(WorkBuddyKind::Desktop, "", "uid", "d", std::chrono::milliseconds(1000)).error == "auth",
            "空 token 未被拒绝");
        Expect(FetchWorkBuddyCredits(WorkBuddyKind::Desktop, std::string(9000, 'a'), "uid", "d",
                                     std::chrono::milliseconds(1000)).error == "auth", "超长 token 未被拒绝");
        Expect(RefreshWorkBuddyToken(WorkBuddyKind::Desktop, "", "uid", "d",
                                     std::chrono::milliseconds(1000)).error == "expired", "空刷新令牌未被拒绝");
        Expect(PollWorkBuddyLogin(WorkBuddyKind::Desktop, "", std::chrono::milliseconds(1000)).error == "parse",
            "空 state 未被拒绝");
    }
    {
        // The two builds are separate services: a login for the AI build must
        // open the AI build's own sign-in page, not the desktop one's. This is
        // the check that the kind argument actually reaches the request.
        const auto desktop = BeginWorkBuddyLogin(WorkBuddyKind::Desktop, std::chrono::milliseconds(8000));
        const auto ai = BeginWorkBuddyLogin(WorkBuddyKind::DesktopAi, std::chrono::milliseconds(8000));
        if (desktop.authUrl.empty() || ai.authUrl.empty()) {
            // No network in this environment: nothing to compare, and the shape
            // checks above already covered the parsing.
            std::cout << "workbuddy: login origins skipped (network unavailable)\n";
        } else {
            Expect(desktop.authUrl.rfind("https://www.codebuddy.cn/login?", 0) == 0,
                   "桌面版登录页应属于 codebuddy.cn");
            Expect(ai.authUrl.rfind("https://www.workbuddy.ai/login", 0) == 0,
                   "AI 版登录页应属于 workbuddy.ai，而不是桌面版");
            Expect(desktop.authUrl != ai.authUrl, "两个版本不应打开同一个登录页");
            std::cout << "  desktop login: " << desktop.authUrl.substr(0, 40) << "\n";
            std::cout << "  ai login     : " << ai.authUrl.substr(0, 40) << "\n";
        }
    }
    {
        // The account id is read from the token's own payload. The AI build's
        // handshake answers with tokens but no uid field, and dropping that
        // sign-in would lose an account the user just authorised.
        const auto token = "h." "eyJzdWIiOiI1MjJiZTgzYi0zZjJjLTQ5OTctODFkZS05OTYwMWZiMzI2YTciLCJuaWNrbmFtZSI6IngifQ" ".s";
        Expect(AccountIdOf(token) == "522be83b-3f2c-4997-81de-99601fb326a7", "应从令牌恢复账号 id");
        // Anything that is not a readable token yields nothing rather than a
        // guess; the caller then reports the failure instead of storing junk.
        Expect(AccountIdOf("").empty(), "空令牌不应给出 id");
        Expect(AccountIdOf("nodots").empty(), "无分隔的令牌不应给出 id");
        Expect(AccountIdOf("h.not-base64!.s").empty(), "非 base64 段不应给出 id");
        Expect(AccountIdOf("h." "eyJhIjoxfQ" ".s").empty(), "没有 sub 的载荷不应给出 id");
    }
    std::cout << "workbuddy: parse and validation checks passed\n";
    return 0;
}