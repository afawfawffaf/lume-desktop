#include "core/ui/music_library_protocol.h"
#include "vendor/nlohmann/json.hpp"
#include <set>

namespace lume::ui {
std::optional<MusicLibraryCommand> ParseMusicLibraryCommand(std::wstring_view message) noexcept {
    try {
        if (message.size() > 2048) return {};
        bool invalid=false;
        std::set<std::string> keys;
        const auto json = nlohmann::json::parse(message.begin(),message.end(),
            [&](int depth,nlohmann::json::parse_event_t event,nlohmann::json& value) {
                if(depth>1)invalid=true;
                if(event==nlohmann::json::parse_event_t::key&&!keys.insert(value.get<std::string>()).second)
                    invalid=true;
                return true;
            },false);
        if (invalid || !json.is_object() || json.value("scope", "") != "music-library" ||
            !json.at("version").is_number_integer() || json.at("version") != 1 ||
            !json.at("action").is_string()) return {};
        MusicLibraryCommand command;
        command.action = json.at("action").get<std::string>();
        if (command.action == "snapshot") {
            if (json.size() != 3) return {};
        } else if (command.action == "import-folder") {
            // The native owner runs the folder picker; no path crosses the page boundary.
            if (json.size() != 3) return {};
        } else if (command.action == "sync-lx") {
            // The native owner owns the playlist and the destination directory;
            // the page cannot steer either one.
            if (json.size() != 3) return {};
        } else if (command.action == "page") {
            // A query is optional so an unfiltered page keeps the shorter shape.
            if ((json.size() != 5 && json.size() != 6) || !json.at("offset").is_number_integer() ||
                !json.at("limit").is_number_integer()) return {};
            const auto offset = json.at("offset").get<std::int64_t>();
            const auto limit = json.at("limit").get<std::int64_t>();
            if (offset < 0 || offset > 4096 || limit < 1 || limit > 100) return {};
            command.offset = static_cast<std::size_t>(offset);
            command.limit = static_cast<std::size_t>(limit);
            if (json.size() == 6) {
                if (!json.at("query").is_string()) return {};
                command.query = json.at("query").get<std::string>();
                if (command.query.size() > 128) return {};
            }
        } else if (command.action == "visibility") {
            if (json.size() != 4 || !json.at("visible").is_boolean()) return {};
            command.visible = json.at("visible").get<bool>();
        } else if (command.action == "play" || command.action == "enqueue" ||
                   command.action == "insertNext" || command.action == "remove") {
            if (json.size() != 4 || !json.at("id").is_string()) return {};
            command.id = json.at("id").get<std::string>();
            if (command.id.size() <= 6 || command.id.size() > 96 || command.id.compare(0,6,"local-")!=0 ||
                command.id.find_first_not_of("0123456789",6) != std::string::npos)
                return {};
        } else return {};
        return command;
    } catch (...) { return {}; }
}
} // namespace lume::ui
