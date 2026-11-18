#include "apps/shell/quota_protocol.h"

#include "vendor/nlohmann/json.hpp"

#include <algorithm>

namespace lume::shell {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaxMessage = 2048;
constexpr std::size_t kMaxKey = 512;
// A uid is a short opaque token; the bound only keeps the message table small.
constexpr std::size_t kMaxId = 128;

std::string Utf8Of(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (const auto ch : text) {
        // The protocol is ASCII by construction; anything else is a paste of the
        // wrong thing and is rejected rather than transcoded.
        if (static_cast<unsigned>(ch) >= 0x80) return {};
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

bool Printable(std::string_view key) {
    return std::all_of(key.begin(), key.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return byte >= 0x21 && byte <= 0x7e;
    });
}
}  // namespace

std::optional<QuotaCommand> ParseQuotaCommand(std::wstring_view message) noexcept {
    try {
        if (message.empty() || message.size() > kMaxMessage) return std::nullopt;
        const auto ascii = Utf8Of(message);
        if (ascii.empty()) return std::nullopt;
        // The key is a scalar, so anything nested is the wrong message. The
        // callback reports that by returning false, which makes the parser give
        // up on its own: throwing out of it would unwind through the parser's
        // own frames instead.
        int depth = 0;
        const auto json = Json::parse(ascii.begin(), ascii.end(),
            [&](int, Json::parse_event_t event, Json&) {
                if (event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) {
                    if (++depth > 1) return false;
                } else if (event == Json::parse_event_t::object_end || event == Json::parse_event_t::array_end) {
                    --depth;
                }
                return true;
            });
        if (json.is_discarded()) return std::nullopt;
        if (!json.is_object() || json.value("version", 0) != 1 ||
            json.value("scope", std::string{}) != "quota" || !json.contains("action") ||
            !json.at("action").is_string())
            return std::nullopt;
        QuotaCommand command;
        command.action = json.at("action").get<std::string>();
        if (command.action == "set") {
            if (json.size() != 4 || !json.contains("key") || !json.at("key").is_string()) return std::nullopt;
            command.key = json.at("key").get<std::string>();
            if (command.key.empty() || command.key.size() > kMaxKey || !Printable(command.key))
                return std::nullopt;
            return command;
        }
        if (json.size() != 3) return std::nullopt;
        if (command.action != "clear" && command.action != "refresh" && command.action != "snapshot")
            return std::nullopt;
        return command;
    } catch (...) {
        return std::nullopt;
    }
}

std::wstring EncodeQuotaState(bool hasKey, bool running, bool ok, bool available,
                              std::string_view error, std::string_view amount,
                              std::string_view currency) {
    // A key's value never appears here: the page only learns whether one exists.
    const auto json = Json{{"version", 1}, {"type", "quota-state"},
        {"hasKey", hasKey}, {"running", running}, {"ok", ok}, {"available", available},
        {"error", std::string(error)}, {"amount", std::string(amount)}, {"currency", std::string(currency)}};
    const auto text = json.dump();
    return std::wstring(text.begin(), text.end());
}


std::wstring EncodeWorkBuddyState(const std::vector<WorkBuddySlotState>& slots,
                                  bool desktopAvailable, bool aiAvailable) {
    // Accounts are sent as a flat list; the page groups them by build, so adding
    // a build later needs no new message shape. A token is never here.
    Json list = Json::array();
    for (const auto& slot : slots) {
        list.push_back({{"build", slot.build}, {"id", slot.id}, {"connected", slot.connected},
            {"label", slot.label}, {"status", slot.status},
            {"remaining", std::isfinite(slot.remaining) ? slot.remaining : 0.0},
            {"plan", slot.plan}});
    }
    const auto json = Json{{"version", 1}, {"type", "workbuddy-state"},
        {"slots", std::move(list)}, {"desktopAvailable", desktopAvailable},
        {"aiAvailable", aiAvailable}};
    const auto text = json.dump();
    return std::wstring(text.begin(), text.end());
}


std::optional<WorkBuddyCommand> ParseWorkBuddyCommand(std::wstring_view message) noexcept {
    try {
        if (message.empty() || message.size() > kMaxMessage) return std::nullopt;
        const auto ascii = Utf8Of(message);
        if (ascii.empty()) return std::nullopt;
        const auto json = Json::parse(ascii.begin(), ascii.end(), nullptr, false);
        if (json.is_discarded() || !json.is_object() || json.value("version", 0) != 1 ||
            json.value("scope", std::string{}) != "workbuddy" || !json.contains("action") ||
            !json.at("action").is_string())
            return std::nullopt;
        WorkBuddyCommand command;
        command.action = json.at("action").get<std::string>();
        if (command.action == "refresh" || command.action == "snapshot") {
            if (json.size() != 3) return std::nullopt;
            return command;
        }
        if (command.action != "connect" && command.action != "disconnect") return std::nullopt;
        if (!json.contains("build") || !json.at("build").is_string()) return std::nullopt;
        command.build = json.at("build").get<std::string>();
        if (command.build != "desktop" && command.build != "ai") return std::nullopt;
        // "connect" names only the build: the account it becomes is whichever one
        // the browser signs in, so no slot has to be reserved in advance.
        if (command.action == "connect") {
            if (json.size() != 4) return std::nullopt;
            return command;
        }
        // "disconnect" names the account by the uid the API assigned.
        if (json.size() != 5 || !json.contains("id") || !json.at("id").is_string())
            return std::nullopt;
        command.id = json.at("id").get<std::string>();
        if (command.id.empty() || command.id.size() > kMaxId || !Printable(command.id))
            return std::nullopt;
        return command;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace lume::shell
