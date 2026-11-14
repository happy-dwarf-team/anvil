#include "last_start.hpp"

#include "crash_history.hpp"
#include "manifest.hpp"
#include "settings.hpp"

#include <toml++/toml.hpp>

#include <charconv>
#include <sstream>

namespace anvil {
namespace {

constexpr const char* kLists[] = {"disabled_plugins", "allow_ironman", "allow_multiplayer", "enabled_modules",
                                  "disabled_modules"};

template <typename Record> auto* list_of(Record& r, std::string_view key) {
    using List = decltype(&r.disabled_plugins);
    if (key == "disabled_plugins") return &r.disabled_plugins;
    if (key == "allow_ironman") return &r.allow_ironman;
    if (key == "allow_multiplayer") return &r.allow_multiplayer;
    if (key == "enabled_modules") return &r.enabled_modules;
    if (key == "disabled_modules") return &r.disabled_modules;
    return List{nullptr};
}

std::string array_text(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) out += (i == 0 ? "" : ", ") + toml_string(items[i]);
    return out + "]";
}

// A value of the record as setting_toml writes it.
std::optional<std::string> value_text(const toml::node& node) {
    if (const auto* b = node.as_boolean()) return std::string(b->get() ? "true" : "false");
    if (const auto* i = node.as_integer()) return std::to_string(i->get());
    if (const auto* s = node.as_string()) return toml_string(s->get());
    if (const auto* f = node.as_floating_point()) {
        char text[64];
        const auto [end, ec] = std::to_chars(text, text + sizeof(text), f->get());
        std::string out(text, ec == std::errc() ? end : text);
        if (out.find_first_of(".en") == std::string::npos) out += ".0";
        return out;
    }
    return std::nullopt;
}

}  // namespace

std::string last_start_text(const LastStart& r) {
    std::string out =
        "# What the last start of the game used (ADR 0013): the lists of anvil.toml, the approvals of trust.toml\n"
        "# and the plugins' settings as that start read them. Anvil writes it at every start; do not edit it.\n";
    out += "format = " + std::to_string(kLastStartFormat) + "\n";
    out += "anvil = " + toml_string(r.anvil_version) + "\n";
    out += "written = " + toml_string(r.written) + "\n";
    out += std::string("safe_mode = ") + (r.safe_mode ? "true" : "false") + "\n";
    out += "\n[anvil_toml]\n";
    for (const char* key : kLists) out += std::string(key) + " = " + array_text(*list_of(r, key)) + "\n";
    for (const auto& a : r.approvals) {
        out += "\n[[approval]]\nid = " + toml_string(a.id) + "\nmod = " + toml_string(a.mod) +
               "\nversion = " + toml_string(a.version) + "\nhash = " + toml_string(a.hash) + "\n";
    }
    for (const auto& [plugin, values] : r.settings) {
        out += "\n[settings." + toml_string(plugin) + "]\n";
        for (const auto& [name, value] : values) out += name + " = " + value + "\n";
    }
    return out;
}

std::optional<LastStart> parse_last_start(std::string_view text, std::string& error) {
    toml::table t;
    try {
        t = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::ostringstream message;
        message << "line " << e.source().begin.line << ": " << e.description();
        error = message.str();
        return std::nullopt;
    }
    if (t["format"].value<int64_t>() != static_cast<int64_t>(kLastStartFormat)) {
        error = "not format " + std::to_string(kLastStartFormat);
        return std::nullopt;
    }
    LastStart r;
    r.anvil_version = t["anvil"].value_or<std::string>("");
    r.written = t["written"].value_or<std::string>("");
    r.safe_mode = t["safe_mode"].value_or(false);
    if (const auto* lists = t["anvil_toml"].as_table()) {
        for (const char* key : kLists) {
            if (const auto* array = lists->get_as<toml::array>(key)) {
                for (const auto& item : *array) {
                    if (auto v = item.value<std::string>()) list_of(r, key)->push_back(*v);
                }
            }
        }
    }
    if (const auto* approvals = t["approval"].as_array()) {
        for (const auto& node : *approvals) {
            const auto* a = node.as_table();
            if (a == nullptr) continue;
            r.approvals.push_back({(*a)["id"].value_or<std::string>(""), (*a)["mod"].value_or<std::string>(""),
                                   (*a)["version"].value_or<std::string>(""), (*a)["hash"].value_or<std::string>("")});
        }
    }
    if (const auto* settings = t["settings"].as_table()) {
        for (const auto& [plugin, node] : *settings) {
            const auto* values = node.as_table();
            if (values == nullptr) continue;
            auto& out = r.settings[std::string(plugin.str())];
            for (const auto& [name, value] : *values) {
                if (auto v = value_text(value)) out[std::string(name.str())] = *v;
            }
        }
    }
    return r;
}

std::optional<LastStart> read_last_start(const std::filesystem::path& file) {
    std::optional<std::string> text = read_text_file(file, kMaxPluginTomlBytes);
    if (!text) return std::nullopt;
    std::string error;
    return parse_last_start(*text, error);
}

bool write_last_start(const std::filesystem::path& file, const LastStart& record, std::string& error) {
    return replace_text_file(file, last_start_text(record), error);
}

}  // namespace anvil
