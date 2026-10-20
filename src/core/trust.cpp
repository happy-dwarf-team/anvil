#include "trust.hpp"

#include "files.hpp"
#include "log.hpp"
#include "manifest.hpp"
#include "settings.hpp"

#include <toml++/toml.hpp>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace anvil {

namespace {

const std::set<std::string, std::less<>> kKeys = {"id", "version", "mod", "mod_name", "workshop_id", "hash", "approved"};

struct Fail {
    std::string message;
    uint32_t line = 0;
};

uint32_t line_of(const toml::source_region& source) { return static_cast<uint32_t>(source.begin.line); }

std::string text_of(const toml::table& t, const char* key, uint32_t line) {
    const toml::node* node = t.get(key);
    if (node == nullptr) throw Fail{std::string("missing key '") + key + "'", line};
    const auto value = node->value<std::string>();
    if (!value || !node->is_string()) throw Fail{std::string(key) + " must be a string", line_of(node->source())};
    return *value;
}

bool is_hash(std::string_view text) {
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

std::string date_text(const toml::date_time& dt) {
    char text[40];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", dt.date.year, dt.date.month, dt.date.day,
                  dt.time.hour, dt.time.minute, dt.time.second);
    return text;
}

}  // namespace

const TrustEntry* TrustFile::find(std::string_view id) const {
    for (const TrustEntry& e : entries) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

TrustFile parse_trust_text(std::string_view text) {
    TrustFile file;
    file.found = true;
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error& e) {
        file.error = std::string(e.description());
        file.line = line_of(e.source());
        return file;
    }
    try {
        for (const auto& [key, node] : table) {
            if (key.str() != "plugin") throw Fail{"unknown key '" + std::string(key.str()) + "'", line_of(node.source())};
        }
        if (const toml::node* node = table.get("plugin")) {
            const toml::array* list = node->as_array();
            if (list == nullptr || !list->is_array_of_tables()) {
                throw Fail{"plugin must be an array of [[plugin]] tables", line_of(node->source())};
            }
            std::set<std::string> seen;
            for (const toml::node& item : *list) {
                const toml::table& t = *item.as_table();
                const uint32_t line = line_of(t.source());
                for (const auto& [key, value] : t) {
                    if (!kKeys.contains(key.str())) {
                        throw Fail{"unknown key '" + std::string(key.str()) + "'", line_of(value.source())};
                    }
                }
                TrustEntry e;
                e.line = line;
                e.id = text_of(t, "id", line);
                if (!is_valid_plugin_id(e.id)) throw Fail{"id '" + e.id + "' is not a plugin id", line};
                if (!seen.insert(e.id).second) throw Fail{"plugin '" + e.id + "' is approved twice", line};
                e.version = text_of(t, "version", line);
                e.mod = text_of(t, "mod", line);
                e.mod_name = text_of(t, "mod_name", line);
                e.workshop_id = text_of(t, "workshop_id", line);
                e.hash = text_of(t, "hash", line);
                if (!is_hash(e.hash)) throw Fail{"hash of '" + e.id + "' is not 64 lower-case hex digits", line};
                const toml::node* approved = t.get("approved");
                if (approved == nullptr) throw Fail{"missing key 'approved'", line};
                if (const auto dt = approved->value<toml::date_time>()) {
                    if (!dt->offset || dt->offset->minutes != 0) {
                        throw Fail{"approved must be in UTC (ending in Z)", line_of(approved->source())};
                    }
                    e.approved = date_text(*dt);
                } else {
                    throw Fail{"approved must be a date-time", line_of(approved->source())};
                }
                file.entries.push_back(std::move(e));
            }
        }
    } catch (const Fail& f) {
        file.entries.clear();
        file.error = f.message;
        file.line = f.line;
    }
    return file;
}

TrustFile read_trust(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return {};
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        TrustFile result;
        result.found = true;
        result.error = "cannot read " + narrow(file.wstring());
        return result;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    return parse_trust_text(text);
}

std::string trust_text(std::vector<TrustEntry> entries) {
    std::sort(entries.begin(), entries.end(), [](const TrustEntry& a, const TrustEntry& b) { return a.id < b.id; });
    std::string out =
        "# Plugins from mods that the player approved (design §14). Anvil writes this\n"
        "# file when a plugin is approved or revoked in the overlay or AnvilManager.exe.\n"
        "# A plugin loads only while its folder hash matches, and only from its\n"
        "# approved copy in anvil/data/anvil/approved/<id>/<hash>/.\n";
    for (const TrustEntry& e : entries) {
        out += "\n[[plugin]]\n";
        out += "id = " + toml_string(e.id) + "\n";
        out += "version = " + toml_string(e.version) + "\n";
        out += "mod = " + toml_string(e.mod) + "\n";
        out += "mod_name = " + toml_string(e.mod_name) + "\n";
        out += "workshop_id = " + toml_string(e.workshop_id) + "\n";
        out += "hash = " + toml_string(e.hash) + "\n";
        out += "approved = " + (e.approved.empty() ? toml_string("") : e.approved) + "\n";
    }
    return out;
}

bool write_trust(const std::filesystem::path& file, const std::vector<TrustEntry>& entries, std::string& error) {
    const TrustFile current = read_trust(file);
    if (!current.error.empty()) {
        error = narrow(file.wstring()) + " does not parse (" + current.error +
                (current.line ? ", line " + std::to_string(current.line) : std::string()) +
                "); fix or remove it first";
        return false;
    }
    for (const TrustEntry& e : entries) {
        if (e.approved.size() != 20 || e.approved[10] != 'T' || e.approved.back() != 'Z') {
            error = "approved of '" + e.id + "' is not a UTC date-time";
            return false;
        }
    }
    const std::string text = trust_text(entries);
    // What is written must read back as the same entries.
    const TrustFile back = parse_trust_text(text);
    if (!back.error.empty() || back.entries.size() != entries.size()) {
        error = "the new trust.toml would not read back (" + back.error + ")";
        return false;
    }
    return write_file_atomic(file, text, error);
}

std::string utc_now() {
    SYSTEMTIME t{};
    GetSystemTime(&t);
    char text[40];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond);
    return text;
}

}  // namespace anvil
