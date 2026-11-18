// The wire format is the only thing this module trusts, so the parser is
// checked directly: no network, no key, no window.
#include "platform/windows/quota/deepseek_balance.h"

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
        // The documented success shape, both currencies.
        const auto body = R"({"is_available":true,"balance_infos":[
            {"currency":"CNY","total_balance":"110.00","granted_balance":"10.00","topped_up_balance":"100.00"},
            {"currency":"USD","total_balance":"15.50","granted_balance":"0.00","topped_up_balance":"15.50"}]})";
        const auto balance = ParseDeepSeekBalance(body, 200);
        Expect(balance.ok && balance.available, "正常响应未被接受");
        Expect(balance.entries.size() == 2, "币种条目数量错误");
        Expect(balance.entries[0].currency == "CNY" && balance.entries[0].total == "110.00", "人民币条目错误");
        Expect(balance.entries[1].currency == "USD" && balance.entries[1].total == "15.50", "美元条目错误");
        Expect(balance.error.empty(), "成功响应不应带错误");
    }
    {
        // An exhausted account is a valid reading, not a failure.
        const auto balance = ParseDeepSeekBalance(
            R"({"is_available":false,"balance_infos":[{"currency":"CNY","total_balance":"0.00",
                "granted_balance":"0.00","topped_up_balance":"0.00"}]})", 200);
        Expect(balance.ok && !balance.available, "零余额应判为可用性 false 而非失败");
        Expect(balance.entries.size() == 1, "零余额条目丢失");
    }
    {
        // Rejected key and server errors must be distinguishable from zero.
        Expect(ParseDeepSeekBalance("{}", 401).error == "auth", "401 未判为鉴权失败");
        Expect(ParseDeepSeekBalance("{}", 403).error == "auth", "403 未判为鉴权失败");
        Expect(ParseDeepSeekBalance("{}", 500).error == "network", "500 未判为网络失败");
        Expect(!ParseDeepSeekBalance("{}", 401).ok, "失败结果不得标记为 ok");
    }
    {
        // Malformed bodies never become a balance.
        Expect(ParseDeepSeekBalance("not json", 200).error == "parse", "非 JSON 未被拒绝");
        Expect(ParseDeepSeekBalance(R"({"is_available":true})", 200).error == "parse", "缺字段未被拒绝");
        Expect(ParseDeepSeekBalance(R"({"is_available":"yes","balance_infos":[]})", 200).error == "parse",
            "is_available 非布尔未被拒绝");
        Expect(ParseDeepSeekBalance(R"({"is_available":true,"balance_infos":[]})", 200).error == "parse",
            "空条目未被拒绝");
        Expect(ParseDeepSeekBalance(
            R"({"is_available":true,"balance_infos":[{"currency":"EUR","total_balance":"1.00"}]})", 200).error == "parse",
            "未知币种未被拒绝");
        Expect(ParseDeepSeekBalance(
            R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"abc"}]})", 200).error == "parse",
            "非数字金额未被拒绝");
        Expect(ParseDeepSeekBalance(
            R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"1.00"},{"currency":"EUR","total_balance":"2.00"}]})",
            200).ok, "混合币种应保留可识别的那条");
    }
    {
        // A key that is empty, binary or oversized is rejected before any request.
        DeepSeekBalance seen;
        FetchDeepSeekBalance("", std::chrono::milliseconds(1000), {}, [&](DeepSeekBalance b) { seen = std::move(b); });
        Expect(seen.error == "key" && !seen.ok, "空密钥未被拒绝");
        seen = {};
        FetchDeepSeekBalance("bad key with spaces", std::chrono::milliseconds(1000), {},
            [&](DeepSeekBalance b) { seen = std::move(b); });
        Expect(seen.error == "key", "含空格的密钥未被拒绝");
        seen = {};
        FetchDeepSeekBalance(std::string(600, 'a'), std::chrono::milliseconds(1000), {},
            [&](DeepSeekBalance b) { seen = std::move(b); });
        Expect(seen.error == "key", "超长密钥未被拒绝");
    }
    std::cout << "deepseek balance: parse and key checks passed\n";
    return 0;
}
