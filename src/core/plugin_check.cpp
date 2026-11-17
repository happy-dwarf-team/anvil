#include "plugin_check.hpp"

#include "discovery.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>

namespace anvil {
namespace {

bool read_file(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    if (out.starts_with("\xEF\xBB\xBF")) out.erase(0, 3);  // saved by Notepad, as the parsers allow
    return true;
}

std::filesystem::path utf8_path(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }

// A path as UTF-8 with forward slashes. generic_string() would convert to the
// ANSI code page and throw on a character it lacks (a Japanese name on a
// Western system), which would end the whole start-up.
std::string generic_utf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

// The parsers start their errors with the file's name; a problem names it apart.
std::string without_prefix(std::string message, std::string_view prefix) {
    if (message.starts_with(prefix)) message.erase(0, prefix.size());
    return message;
}

// A problem of a symbol in the selected build: at its table for that build, if it has one.
uint32_t line_in_build(const Symbol& s) { return s.placement_line != 0 ? s.placement_line : s.line; }

void set_skip(PluginCheck& check, i18n::Message why) {
    if (check.skip.empty()) check.skip = std::move(why);
}

// symbols.toml against the game: its build, then every symbol (design §7).
void check_symbols(PluginCheck& check, SymbolFile& file, const std::string& name, const GameImage& game) {
    if (game.reader == nullptr) return;
    if (!game.image) {
        check.problems.push_back({name, 0, game.exe_name + ": " + game.image_error});
        set_skip(check, i18n::Message("image_unreadable", {game.image_error}));
        return;
    }
    if (game.build_id != nullptr) {
        check.build_declared = select_build(file, game.build_id);
        if (!check.build_declared) {
            check.problems.push_back({name, file.build_line,
                                      "symbols.toml is for " + file.build_list() + ", " + game.exe_name + " is " +
                                          game.build_id});
            set_skip(check, build_mismatch(file, game.build_id));
        }
    } else {
        check.problems.push_back({name, file.build_line,
                                  game.exe_name +
                                      (game.exe_sha256.empty() ? std::string() : " (SHA-256 " + game.exe_sha256 + ")") +
                                      " is not a game build this Anvil knows; symbols.toml declares " + file.build_list()});
        set_skip(check, game.unknown_build);
    }
    // Every symbol of the selected build (the game's, else the file's first),
    // so a check lists each one that fails; the loader skips at the first.
    check.verification = verify_symbols(file, *game.image);
    for (size_t i = 0; i < check.verification.problems.size(); ++i) {
        const Symbol& s = file.symbols[check.verification.failed[i]];
        check.problems.push_back({name, line_in_build(s),
                                  (file.builds_listed ? "in build " + file.build + ": " : std::string()) +
                                      check.verification.problems[i]});
    }
    if (!check.verification.problems.empty()) set_skip(check, symbol_mismatch(check.verification.problems));
    check.verified = check.skip.empty();
}

}  // namespace

GameImage game_image(const ImageReader& reader, const char* build_id, i18n::Message unknown_build) {
    GameImage game;
    game.reader = &reader;
    game.image = PeImage::parse(reader, game.image_error);
    game.build_id = build_id;
    game.unknown_build = std::move(unknown_build);
    return game;
}

PluginCheck check_plugin_folder(const std::filesystem::path& folder, const GameImage& game) {
    PluginCheck check;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(folder / "plugin.toml", ec)) {
        check.manifest_missing = true;
        check.manifest = load_manifest(folder);
        check.problems.push_back({"plugin.toml", 0, "plugin.toml not found"});
        check.skip = i18n::Message(check.manifest.error);
        return check;
    }
    check.manifest = load_manifest(folder);
    if (!check.manifest.manifest) {
        check.problems.push_back({"plugin.toml", check.manifest.line, without_prefix(check.manifest.error, "plugin.toml: ")});
        check.skip = i18n::Message(check.manifest.error);
        return check;
    }
    const Manifest& m = *check.manifest.manifest;
    check.entry_missing = missing_entry_reason(m);
    if (!m.symbols.empty()) {
        const std::filesystem::path relative = utf8_path(m.symbols);
        const std::string name = generic_utf8(relative);
        std::string text;
        if (!read_file(folder / relative, text)) {
            // The plugin cannot load, but its settings.toml is still read: the
            // player's values and retired settings of a plugin that does not
            // load are kept and moved (ADR 0013).
            check.unreadable = name;
            set_skip(check, i18n::Message("cannot_read", {name}));
        } else if (SymbolFileResult parsed = parse_symbols_text(text); !parsed.file) {
            check.problems.push_back({name, parsed.line, without_prefix(parsed.error, "symbols.toml: ")});
            set_skip(check, i18n::Message(parsed.error));
        } else {
            check_symbols(check, *parsed.file, name, game);
            check.symbols = std::move(parsed.file);
        }
    }
    // settings.toml: the loader skips a plugin whose declarations do not
    // parse, as it could not read its settings (design §11).
    if (!m.settings.empty()) {
        const std::filesystem::path relative = utf8_path(m.settings);
        const std::string name = generic_utf8(relative);
        std::string text;
        if (!read_file(folder / relative, text)) {
            if (check.unreadable.empty()) check.unreadable = name;
            set_skip(check, i18n::Message("cannot_read", {name}));
            return check;
        }
        SettingsFileResult parsed = parse_settings_text(text);
        if (!parsed.file) {
            check.problems.push_back({name, parsed.line, without_prefix(parsed.error, "settings.toml: ")});
            set_skip(check, i18n::Message(parsed.error));
        } else {
            uint32_t line = 0;  // `module` names a [[modules]] id (ADR 0013)
            if (const std::string error = settings_module_error(*parsed.file, m, line); !error.empty()) {
                check.problems.push_back({name, line, without_prefix(error, "settings.toml: ")});
                set_skip(check, i18n::Message(error));
            } else {
                check.settings_fit = true;
            }
            check.settings = std::move(parsed.file);
        }
    }
    const auto by_place = [](const PluginProblem& a, const PluginProblem& b) {
        return std::tie(a.file, a.line) < std::tie(b.file, b.line);
    };
    std::stable_sort(check.problems.begin(), check.problems.end(), by_place);
    return check;
}

}  // namespace anvil
