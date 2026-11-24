// Generated and mutated inputs for the parsers that read modders' and
// players' files: anvil-map's map mode files, the .mod descriptor and
// dlc_load.json (anvil-map's readers against the loader's), and the
// anvil.toml list editor. Deterministic: a fixed seed, so a failure repeats.
// ANVIL_FUZZ_ITERATIONS (default 2000 per target) and ANVIL_FUZZ_SEED widen a
// local run; a failing case is printed in full.
#include "../plugins/anvil-map/src/files.hpp"
#include "core/anvil_toml_edit.hpp"
#include "core/playset.hpp"

#include <toml++/toml.hpp>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
int g_shown = 0;

std::string printable(std::string_view text) {
    std::string out;
    for (const unsigned char c : text) {
        if (c == '\n') {
            out += "\\n\n    ";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c < 0x20 || c >= 0x7F) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\x%02X", c);
            out += b;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

// A failed property: counted, and its input printed (the first few only).
void fail(const char* target, const std::string& what, std::string_view input) {
    ++g_failures;
    if (++g_shown > 4) return;  // per target
    std::printf("  FAILED %s: %s\n  input:\n    %s\n", target, what.c_str(), printable(input).c_str());
}

void check(bool ok, const char* target, const std::string& what, std::string_view input) {
    ++g_checks;
    if (!ok) fail(target, what, input);
}

std::size_t env_number(const char* name, std::size_t fallback) {
    char buffer[32] = {};
    const DWORD n = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (n == 0 || n >= sizeof(buffer)) return fallback;
    return static_cast<std::size_t>(std::strtoull(buffer, nullptr, 10));
}

class Gen {
public:
    explicit Gen(std::uint64_t seed) : rng_(seed) {}
    std::size_t below(std::size_t n) { return n == 0 ? 0 : std::uniform_int_distribution<std::size_t>(0, n - 1)(rng_); }
    bool chance(int percent) { return below(100) < static_cast<std::size_t>(percent); }
    template <class T>
    const T& pick(const std::vector<T>& v) { return v[below(v.size())]; }
    std::string word(std::size_t max = 8) {
        static const std::string_view letters = "abcdefghijklmnopqrstuvwxyz";
        std::string w(1 + below(max), 'a');
        for (char& c : w) c = letters[below(letters.size())];
        return w;
    }
    // Whitespace between tokens: spaces, tabs, newlines, CRLF, comments.
    std::string gap() {
        switch (below(8)) {
            case 0: return "\n";
            case 1: return "\r\n";
            case 2: return "\t";
            case 3: return "  ";
            case 4: return " # " + word() + "\n";
            case 5: return "\n\n";
            default: return " ";
        }
    }
    // Random edits from bytes the parsers treat specially.
    std::string mutate(std::string text, std::size_t edits) {
        static constexpr char kSpecial[] = "{}=\"#\\\n\r[],:'<>!\t \xEF\xBB\xBF\xC3\xA9\xFF\x00";
        static constexpr std::string_view special(kSpecial, sizeof(kSpecial) - 1);  // with the NUL
        for (std::size_t i = 0; i < edits; ++i) {
            const std::size_t at = below(text.size() + 1);
            const char c = chance(70) ? special[below(special.size())] : static_cast<char>(below(256));
            switch (below(3)) {
                case 0: text.insert(text.begin() + static_cast<std::ptrdiff_t>(at), c); break;
                case 1: if (at < text.size()) text.erase(at, 1 + below(4)); break;
                default: if (at < text.size()) text[at] = c; break;
            }
        }
        return text;
    }

private:
    std::mt19937_64 rng_;
};

// ---- map mode files -------------------------------------------------------------

struct GenMode {
    std::string name;
    std::string text;
};

std::string label_value(Gen& g) {
    return g.chance(50) ? g.word() : "\"" + g.word() + " " + g.word() + " #" + g.word() + " {x}\"";
}

std::string valid_rule(Gen& g) {
    static const std::vector<std::string> tests = {
        "always = yes", "flag = my_flag", "variable = my_var at_least = 5", "hre = yes", "coastal = no",
        "value = development at_least = 10", "owned_by = RUS", "religion = orthodox owner = yes",
        "subject = yes owner = yes", "modifier = local_tax_modifier above = 0.1", "state = yes",
        "area = moscow_area", "core = yes", "trade_goods = grain"};
    std::string r = "rule = {" + g.gap() + g.pick(tests) + g.gap() + "color = { " + std::to_string(g.below(256)) +
                    " " + std::to_string(g.below(256)) + " " + std::to_string(g.below(256)) + " }" + g.gap() +
                    "label = " + label_value(g) + g.gap() + "}";
    return r;
}

GenMode valid_mode(Gen& g, std::size_t index) {
    GenMode m;
    m.name = "m" + std::to_string(index) + "_" + g.word(4);
    std::vector<std::string> keys = {"label = " + label_value(g)};
    if (g.chance(40)) keys.push_back("description = \"" + g.word() + " " + g.word() + "\"");
    if (g.chance(30)) keys.push_back("tab = " + std::to_string(1 + g.below(4)));
    if (g.chance(20)) keys.push_back(std::string("legend = ") + (g.chance(50) ? "yes" : "no"));
    const std::size_t rules = 1 + g.below(3);
    for (std::size_t i = 0; i < rules; ++i) keys.push_back(valid_rule(g));
    std::shuffle(keys.begin() + 1, keys.end(), std::mt19937(static_cast<unsigned>(g.below(1u << 30))));
    m.text = m.name + " = {";
    for (const std::string& k : keys) m.text += g.gap() + k;
    m.text += g.gap() + "}";
    return m;
}

// A fault inside the mode's block, which leaves that mode out and no other.
std::string faulty_mode(Gen& g, const GenMode& m) {
    static const std::vector<std::string> faults = {
        "rule = { nope = 1 color = { 0 0 0 } label = x }",
        "rule = { always = yes label = x }",
        "rule = { variable = v >= 5 color = { 0 0 0 } label = x }",
        "label = \"no end\n",
        "description = \"two\nlines\"",
        "description = \"two\nlines\" }\n",  // closes the block itself: the rest of the mode is read as junk
        "tab = 9",
        "rule = { always = yes color = { 0 0 0 } label = x junk = { a = { b = c } } }",
        "label = \"open { brace\n",
        "rule = { always = no color = { 0 0 0 } label = x }",
        "rule = { always = yes color = { 0 0 300 } label = x }",
        "= = =",
    };
    const std::string& f = g.pick(faults);
    const std::size_t open = m.text.find('{');
    return m.text.substr(0, open + 1) + g.gap() + f + g.gap() + m.text.substr(open + 1);
}

std::set<std::string> names_of(const std::vector<anvil_map::files::FileMode>& modes) {
    std::set<std::string> out;
    for (const auto& m : modes) out.insert(m.name);
    return out;
}

void map_mode_files(std::size_t iterations, std::uint64_t seed) {
    Gen g(seed);
    using namespace anvil_map::files;
    for (std::size_t it = 0; it < iterations; ++it) {
        const std::size_t count = 1 + g.below(5);
        std::vector<GenMode> modes;
        for (std::size_t i = 0; i < count; ++i) modes.push_back(valid_mode(g, i));
        std::string text = g.chance(10) ? "\xEF\xBB\xBF" : "";
        for (const GenMode& m : modes) text += m.text + g.gap();

        // Valid modes: every one is read, with no fault.
        std::vector<FileMode> out;
        std::vector<std::string> problems;
        parse(text, "f.txt", out, problems);
        check(problems.empty() && out.size() == count, "map file (valid)",
              problems.empty() ? std::to_string(out.size()) + " of " + std::to_string(count) + " modes read"
                               : problems[0],
              text);

        // One fault in one mode: a line for it, and every other mode is read.
        const std::size_t bad = g.below(count);
        std::string faulty;
        for (std::size_t i = 0; i < count; ++i) faulty += (i == bad ? faulty_mode(g, modes[i]) : modes[i].text) + "\n";
        out.clear();
        problems.clear();
        parse(faulty, "f.txt", out, problems);
        const std::set<std::string> read = names_of(out);
        std::string missing;
        for (std::size_t i = 0; i < count; ++i) {
            if (i != bad && !read.contains(modes[i].name)) missing += " " + modes[i].name;
        }
        check(!problems.empty(), "map file (one fault)", "no fault line", faulty);
        check(missing.empty(), "map file (one fault)", "left out without a fault of its own:" + missing, faulty);

        // Random edits: no exception, at most kFaultsShown lines and a count, in time.
        const std::string mutated = g.mutate(text, 1 + g.below(12));
        out.clear();
        problems.clear();
        const auto start = std::chrono::steady_clock::now();
        try {
            parse(mutated, "f.txt", out, problems);
        } catch (const std::exception& e) {
            fail("map file (edits)", std::string("exception: ") + e.what(), mutated);
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        check(problems.size() <= kFaultsShown + 1, "map file (edits)", std::to_string(problems.size()) + " lines", mutated);
        check(ms.count() < 500, "map file (edits)", std::to_string(ms.count()) + " ms", mutated);
    }
}

// ---- .mod descriptors: anvil-map against the loader -----------------------------

std::wstring wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

// A quoted text whose quotes all close: escapes, #, braces, = and newlines inside.
std::string quoted(Gen& g, std::string& value) {
    static const std::vector<std::string> parts = {"a", "mod/x", " ", "#", "{", "}", "=", "\n", "\\\"", "\\\\",
                                                   "C:/mods/", "\xC3\xA9", "path", "name"};
    std::string q = "\"";
    value.clear();
    const std::size_t n = g.below(6);
    for (std::size_t i = 0; i < n; ++i) {
        const std::string& p = g.pick(parts);
        q += p;
        value += p == "\\\"" ? "\"" : p == "\\\\" ? "\\" : p;
    }
    return q + "\"";
}

std::string descriptor(Gen& g) {
    std::string text = g.chance(10) ? "\xEF\xBB\xBF" : "";
    const std::size_t n = 1 + g.below(7);
    for (std::size_t i = 0; i < n; ++i) {
        std::string value;
        switch (g.below(8)) {
            case 0: text += "name = " + quoted(g, value); break;
            case 1: text += "path = " + quoted(g, value); break;
            case 2: text += "path=" + g.word() + "/" + g.word(); break;  // a bare value
            case 3: text += "tags = {" + g.gap() + quoted(g, value) + g.gap() + "path = " + quoted(g, value) + " }"; break;
            case 4: text += "archive = " + quoted(g, value); break;
            case 5: text += "replace_path = " + quoted(g, value); break;
            case 6: text += "# path = " + quoted(g, value) + "\n"; break;
            default: text += g.word() + " = " + quoted(g, value); break;
        }
        text += g.chance(80) ? g.gap() : "";
    }
    return text;
}

void descriptors(std::size_t iterations, std::uint64_t seed) {
    Gen g(seed);
    for (std::size_t it = 0; it < iterations; ++it) {
        const std::string text = descriptor(g);
        // The loader takes a BOM off when it reads the file, before parsing.
        std::string_view body = text;
        if (body.starts_with("\xEF\xBB\xBF")) body.remove_prefix(3);
        const anvil::PlaysetMod loader = anvil::parse_mod_descriptor(body);
        std::string path = anvil_map::files::mod_value(text, "path");
        std::string name = anvil_map::files::mod_value(text, "name");
        // A quoted text left open at the end of the file (a # comment ended by a
        // newline inside the generated text leaves one): the loader takes it to
        // the end, anvil-map takes none. The game loads no such mod either
        // (anvil#545 T7), so no map mode file is read from it either way. When
        // closing that quote gives the loader's value, they agree.
        const std::string closed = text + "\"";
        if (path.empty() && !loader.folder.empty()) path = anvil_map::files::mod_value(closed, "path");
        if (name.empty() && !loader.name.empty()) name = anvil_map::files::mod_value(closed, "name");
        check(name == loader.name, "descriptor", "name: anvil-map '" + name + "', loader '" + loader.name + "'", text);
        check(std::filesystem::path(wide(path)) == loader.folder, "descriptor",
              "path: anvil-map '" + path + "', loader " + std::to_string(loader.folder.u8string().size()) + " bytes",
              text);

        const std::string mutated = g.mutate(text, 1 + g.below(8));
        try {
            (void)anvil_map::files::mod_value(mutated, "path");
            (void)anvil::parse_mod_descriptor(mutated);
        } catch (const std::exception& e) {
            fail("descriptor (edits)", std::string("exception: ") + e.what(), mutated);
        }
    }
}

// ---- dlc_load.json: anvil-map against the loader --------------------------------

std::string json_gap(Gen& g) {
    static const std::vector<std::string> gaps = {"", " ", "\n", "\r\n", "\t", "\n  "};
    return g.pick(gaps);
}

std::string json_string(Gen& g, std::string& value) {
    static const std::vector<std::pair<std::string, std::string>> parts = {
        {"mod/", "mod/"}, {"ugc_", "ugc_"}, {"123", "123"}, {".mod", ".mod"}, {"]", "]"}, {"[", "["},
        {"\\\"", "\""}, {"\\\\", "\\"}, {"\\/", "/"}, {"\\u00e9", "\xC3\xA9"}, {"\xC3\xA9", "\xC3\xA9"},
        {",", ","}, {" ", " "}, {"enabled_mods", "enabled_mods"}};
    std::string s = "\"";
    value.clear();
    const std::size_t n = 1 + g.below(6);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& p = g.pick(parts);
        s += p.first;
        value += p.second;
    }
    return s + "\"";
}

std::string json_value(Gen& g, int depth) {
    std::string ignored;
    switch (depth > 2 ? g.below(3) : g.below(6)) {
        case 0: return json_string(g, ignored);
        case 1: return std::to_string(g.below(1000));
        case 2: return g.chance(50) ? "true" : "null";
        case 3: return "[" + json_value(g, depth + 1) + "," + json_value(g, depth + 1) + "]";
        case 4: return "{" + json_string(g, ignored) + ":" + json_value(g, depth + 1) + "}";
        default: return "[]";
    }
}

void dlc_load(std::size_t iterations, std::uint64_t seed) {
    Gen g(seed);
    for (std::size_t it = 0; it < iterations; ++it) {
        std::vector<std::string> members;
        const std::size_t mods = g.below(5);
        std::string list = "[";
        for (std::size_t i = 0; i < mods; ++i) {
            std::string value;
            list += (i ? "," + json_gap(g) : json_gap(g)) + json_string(g, value);
        }
        list += json_gap(g) + "]";
        members.push_back("\"enabled_mods\":" + json_gap(g) + list);
        const std::size_t others = g.below(3);
        for (std::size_t i = 0; i < others; ++i) {
            std::string key;
            const std::string member = json_string(g, key) + ":" + json_value(g, 0);
            if (key != "enabled_mods") members.push_back(member);  // a second list would replace the first
        }
        if (g.chance(30)) members.push_back("\"disabled_dlcs\":[]");
        std::shuffle(members.begin(), members.end(), std::mt19937(static_cast<unsigned>(g.below(1u << 30))));
        std::string text = g.chance(10) ? "\xEF\xBB\xBF{" : "{";
        for (std::size_t i = 0; i < members.size(); ++i) text += (i ? "," : "") + json_gap(g) + members[i];
        text += json_gap(g) + "}";

        std::string error;
        const auto loader = anvil::parse_dlc_load(text, error);
        if (!loader) {
            fail("dlc_load.json", "the loader refuses a generated file: " + error, text);
            continue;
        }
        const std::vector<std::string> map = anvil_map::files::enabled_mods(text);
        std::string differ;
        if (map != *loader) {
            differ = "anvil-map reads " + std::to_string(map.size()) + " mods, the loader " +
                     std::to_string(loader->size());
            for (std::size_t i = 0; i < std::min(map.size(), loader->size()); ++i) {
                if (map[i] != (*loader)[i]) {
                    differ += "; first difference '" + map[i] + "' / '" + (*loader)[i] + "'";
                    break;
                }
            }
        }
        check(differ.empty(), "dlc_load.json", differ, text);

        const std::string mutated = g.mutate(text, 1 + g.below(8));
        try {
            (void)anvil_map::files::enabled_mods(mutated);
            (void)anvil::parse_dlc_load(mutated, error);
        } catch (const std::exception& e) {
            fail("dlc_load.json (edits)", std::string("exception: ") + e.what(), mutated);
        }
    }
}

// ---- anvil.toml: the list editor ------------------------------------------------

const std::vector<std::string> kLists = {"disabled_plugins", "allow_ironman", "allow_multiplayer", "enabled_modules",
                                         "disabled_modules"};

std::string toml_id(Gen& g, const std::string& list) {
    static const std::vector<std::string> plugins = {"tongs", "bellows", "my-plugin", "a", "anvil-map", "x1"};
    static const std::vector<std::string> modules = {"tongs.game_calls", "bellows.memo", "a.b", "my-plugin.m_2"};
    if (list == "disabled_plugins") return g.pick(plugins);
    if (list == "enabled_modules" || list == "disabled_modules") return g.pick(modules);
    return g.chance(50) ? g.pick(plugins) : g.pick(modules);
}

std::string toml_item(Gen& g, const std::string& id) {
    switch (g.below(4)) {
        case 0: return "'" + id + "'";
        case 1: return "\"" + id + "\"";
        case 2: return "\"" + id + "\"  ";
        default: return "\"" + id + "\"";
    }
}

std::string toml_list(Gen& g, const std::string& key, std::vector<std::string>& ids) {
    const std::size_t n = g.below(4);
    const bool multiline = g.chance(40);
    std::string s = (g.chance(10) ? "\"" + key + "\"" : key) + (g.chance(50) ? " = " : "=") + "[";
    for (std::size_t i = 0; i < n; ++i) {
        const std::string id = toml_id(g, key);
        ids.push_back(id);
        s += multiline ? "\n  " : (i ? " " : "");
        s += multiline ? toml_item(g, id) : (g.chance(50) ? "'" + id + "'" : "\"" + id + "\"");
        if (i + 1 < n || (multiline && g.chance(50))) s += ",";
        if (multiline && g.chance(30)) s += " # " + g.word();
    }
    s += multiline ? "\n]" : "]";
    return s;
}

std::string toml_file(Gen& g) {
    std::vector<std::string> lines;
    std::vector<std::string> keys = kLists;
    std::shuffle(keys.begin(), keys.end(), std::mt19937(static_cast<unsigned>(g.below(1u << 30))));
    for (const std::string& k : keys) {
        if (!g.chance(60)) continue;
        std::vector<std::string> ids;
        lines.push_back(toml_list(g, k, ids));
    }
    if (g.chance(50)) lines.push_back(std::string("enabled = ") + (g.chance(50) ? "true" : "false"));
    if (g.chance(30)) lines.push_back("log_level = \"debug\"");
    if (g.chance(30)) lines.push_back("# " + g.word() + " = [\"tongs\"]");
    std::shuffle(lines.begin(), lines.end(), std::mt19937(static_cast<unsigned>(g.below(1u << 30))));
    if (g.chance(30)) {
        lines.push_back("[" + g.word() + "]");
        std::vector<std::string> ids;
        lines.push_back(toml_list(g, g.pick(kLists), ids));
    }
    const std::string newline = g.chance(20) ? "\r\n" : "\n";
    std::string text = g.chance(10) ? "\xEF\xBB\xBF" : "";
    for (const std::string& l : lines) text += l + newline;
    return text;
}

std::vector<std::string> list_of(const toml::table& t, const std::string& key) {
    std::vector<std::string> out;
    if (const toml::array* a = t.get_as<toml::array>(key)) {
        for (const auto& item : *a) out.push_back(item.value<std::string>().value_or("<not a string>"));
    }
    return out;
}

void anvil_toml(std::size_t iterations, std::uint64_t seed) {
    Gen g(seed);
    std::size_t refused = 0;
    for (std::size_t it = 0; it < iterations; ++it) {
        const std::string text = toml_file(g);
        const std::string key = g.pick(kLists);
        const std::string id = toml_id(g, key);
        const bool listed = g.chance(50);
        std::string error;
        std::optional<std::string> edited;
        try {
            edited = anvil::with_plugin_listed(text, key, id, listed, error);
        } catch (const std::exception& e) {
            fail("anvil.toml", std::string("exception: ") + e.what(), text);
            continue;
        }
        std::string_view body = text;
        if (body.starts_with("\xEF\xBB\xBF")) body.remove_prefix(3);
        toml::table before;
        try {
            before = toml::parse(body);
        } catch (const toml::parse_error&) {
            fail("anvil.toml", "the generator wrote a file that does not parse", text);
            continue;
        }
        if (!edited) {
            // Refused although the file parses and the key is a list (or absent).
            ++refused;
            fail("anvil.toml", "refused " + std::string(listed ? "adding '" : "removing '") + id + "' to " + key +
                                   ": " + error,
                 text);
            continue;
        }
        std::string_view out = *edited;
        check(out.starts_with("\xEF\xBB\xBF") == text.starts_with("\xEF\xBB\xBF"), "anvil.toml", "BOM changed", text);
        if (out.starts_with("\xEF\xBB\xBF")) out.remove_prefix(3);
        toml::table after;
        try {
            after = toml::parse(out);
        } catch (const toml::parse_error& e) {
            fail("anvil.toml", std::string("the edit does not parse: ") + std::string(e.description()), text);
            continue;
        }
        std::vector<std::string> want = list_of(before, key);
        const bool present = std::find(want.begin(), want.end(), id) != want.end();
        // Adding an id already listed changes nothing, duplicates included.
        if (!(listed && present)) {
            want.erase(std::remove(want.begin(), want.end(), id), want.end());
            if (listed) want.push_back(id);
        }
        const std::vector<std::string> got = list_of(after, key);
        // An id already listed stays where it was; compare as sets of order-free content then.
        std::vector<std::string> a = got, b = want;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        check(a == b, "anvil.toml", std::string(listed ? "add '" : "remove '") + id + "' in " + key + ": wrong list",
              text);
        toml::table rest_before = before, rest_after = after;
        rest_before.erase(key);
        rest_after.erase(key);
        check(rest_before == rest_after, "anvil.toml", "other keys changed", text);
    }
    std::printf("  anvil.toml: %zu edits refused\n", refused);
}

}  // namespace

int main() {
    const std::size_t iterations = env_number("ANVIL_FUZZ_ITERATIONS", 2000);
    const std::uint64_t seed = env_number("ANVIL_FUZZ_SEED", 1125);
    std::printf("seed %llu, %zu iterations per target\n", static_cast<unsigned long long>(seed), iterations);
    const std::vector<std::pair<const char*, std::function<void(std::size_t, std::uint64_t)>>> targets = {
        {"map mode files", map_mode_files},
        {".mod descriptors", descriptors},
        {"dlc_load.json", dlc_load},
        {"anvil.toml list edits", anvil_toml},
    };
    for (const auto& [name, target] : targets) {
        const int before = g_failures;
        g_shown = 0;
        try {
            target(iterations, seed);
        } catch (const std::exception& e) {
            ++g_failures;
            std::printf("  FAILED: exception: %s\n", e.what());
        }
        std::printf("%s %s (%d failed)\n", g_failures == before ? "ok  " : "FAIL", name, g_failures - before);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
