#include "settings.hpp"

#include "manifest.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>

namespace anvil {
namespace {

const std::set<std::string_view> kTopKeys = {"setting", "retired"};
const std::set<std::string_view> kSettingKeys = {"type",  "default", "min",    "max",      "step",
                                                 "values", "label",  "description", "group", "restart",
                                                 "module", "advanced", "affects_gameplay"};
const std::set<std::string_view> kRetiredKeys = {"module", "off", "on", "ironman", "multiplayer"};
const std::set<std::string_view> kOptionKeys = {"name", "label"};

struct Fail {
    std::string message;
    uint32_t line = 0;  // 0: the caller fills in the line of the key it was reading
};

uint32_t line_of(const toml::source_region& source) { return static_cast<uint32_t>(source.begin.line); }

// Setting and option names become C/C++/Rust identifiers in generated headers.
bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || !(name[0] >= 'a' && name[0] <= 'z')) return false;
    return std::all_of(name.begin(), name.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

struct Entry {
    std::string key;
    const toml::node* node;
    uint32_t line;  // of the key
};

// Table entries in the order they appear in the file (toml::table is sorted by key).
std::vector<Entry> in_file_order(const toml::table& table) {
    std::vector<std::pair<const toml::key*, const toml::node*>> entries;
    for (const auto& [key, node] : table) entries.emplace_back(&key, &node);
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        const auto& x = a.first->source().begin;
        const auto& y = b.first->source().begin;
        return x.line != y.line ? x.line < y.line : x.column < y.column;
    });
    std::vector<Entry> out;
    for (const auto& [key, node] : entries) out.push_back({std::string(key->str()), node, line_of(key->source())});
    return out;
}

// Runs `body`; a Fail without a line gets `line`.
template <typename Body> void at_line(uint32_t line, Body&& body) {
    try {
        body();
    } catch (Fail& f) {
        if (f.line == 0) f.line = line;
        throw;
    }
}

std::string get_string(const toml::node& node, std::string_view key, bool allow_empty) {
    const auto* value = node.as_string();
    if (value == nullptr) throw Fail{std::string(key) + " must be a string"};
    if (!allow_empty && value->get().empty()) throw Fail{std::string(key) + " must not be empty"};
    return value->get();
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string format_float(double value) {
    char text[64];
    const auto [end, ec] = std::to_chars(text, text + sizeof(text), value);
    std::string out(text, ec == std::errc() ? end : text);
    if (out.find_first_of(".en") == std::string::npos) out += ".0";  // TOML: a float, not an integer
    return out;
}

}  // namespace

std::string toml_string(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\f': out += "\\f"; break;
        case '\r': out += "\\r"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                char escape[8];
                std::snprintf(escape, sizeof(escape), "\\u%04X", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += escape;
            } else {
                out.push_back(c);
            }
        }
    }
    return out + "\"";
}

namespace {

std::string option_list(const Setting& s) {
    std::string out;
    for (const auto& o : s.options) out += (out.empty() ? "" : ", ") + o.name;
    return out;
}

bool is_numeric(SettingType type) { return type == SettingType::Int || type == SettingType::Float; }

// A value of the setting's type from TOML. Limits are not checked here.
SettingValue value_from_node(const Setting& s, const toml::node& node, std::string_view what) {
    SettingValue v;
    const std::string where(what);
    switch (s.type) {
    case SettingType::Bool:
        if (const auto* b = node.as_boolean()) {
            v.integer = b->get() ? 1 : 0;
            return v;
        }
        throw Fail{where + " must be true or false"};
    case SettingType::Int:
        if (const auto* i = node.as_integer()) {
            v.integer = i->get();
            return v;
        }
        throw Fail{where + " must be an integer"};
    case SettingType::Float:
        if (const auto* f = node.as_floating_point()) v.real = f->get();
        else if (const auto* i = node.as_integer()) v.real = static_cast<double>(i->get());
        else throw Fail{where + " must be a number"};
        if (!std::isfinite(v.real)) throw Fail{where + " must be a finite number"};
        return v;
    case SettingType::Enum: {
        const std::string name = get_string(node, where, true);
        for (size_t i = 0; i < s.options.size(); ++i) {
            if (s.options[i].name == name) {
                v.integer = static_cast<int64_t>(i);
                return v;
            }
        }
        throw Fail{where + ": \"" + name + "\" is not one of " + option_list(s)};
    }
    case SettingType::String:
        v.text = get_string(node, where, true);
        if (v.text.find('\0') != std::string::npos) throw Fail{where + " must not contain a NUL character"};
        return v;
    case SettingType::Key: {
        const std::string text = get_string(node, where, true);
        const auto code = parse_key(text);
        if (!code) throw Fail{where + ": \"" + text + "\" is not a key binding such as \"Ctrl+Shift+F12\""};
        v.integer = *code;
        return v;
    }
    }
    throw Fail{where + ": unknown type"};
}

bool below(const Setting& s, const SettingValue& a, const SettingValue& b) {
    return s.type == SettingType::Int ? a.integer < b.integer : a.real < b.real;
}

std::vector<SettingOption> parse_options(const toml::node& node) {
    const auto* array = node.as_array();
    if (array == nullptr || array->empty()) {
        throw Fail{"values must be a non-empty array such as [{ name = \"off\", label = \"Off\" }]"};
    }
    std::vector<SettingOption> options;
    for (const auto& item : *array) {
        const uint32_t item_line = line_of(item.source());
        const auto* table = item.as_table();
        if (table == nullptr) throw Fail{"each of values must be a table such as { name = \"off\", label = \"Off\" }", item_line};
        SettingOption o;
        bool has_name = false;
        for (const auto& [key, value, line] : in_file_order(*table)) {
            at_line(line, [&] {
                if (!kOptionKeys.contains(key)) throw Fail{"values: unknown key '" + key + "'"};
                if (key == "name") {
                    o.name = get_string(*value, "values.name", false);
                    has_name = true;
                    if (!valid_name(o.name)) {
                        throw Fail{"values: name '" + o.name + "' must match [a-z][a-z0-9_]* (max 64 characters)"};
                    }
                } else {
                    o.label = get_string(*value, "values.label", false);
                }
            });
        }
        if (!has_name) throw Fail{"values: an option without 'name'", item_line};
        for (const auto& other : options) {
            if (other.name == o.name) throw Fail{"values: '" + o.name + "' is declared twice", item_line};
        }
        if (o.label.empty()) o.label = o.name;
        options.push_back(std::move(o));
    }
    return options;
}

Setting parse_setting(const std::string& name, const toml::node& node, uint32_t table_line) {
    const auto* table = node.as_table();
    if (table == nullptr) throw Fail{"must be a table"};
    Setting s;
    s.name = name;
    const std::vector<Entry> entries = in_file_order(*table);
    for (const auto& e : entries) {
        if (!kSettingKeys.contains(e.key)) throw Fail{"unknown key '" + e.key + "'", e.line};
    }
    const auto entry = [&](std::string_view key) -> const Entry* {
        for (const auto& e : entries) {
            if (e.key == key) return &e;
        }
        return nullptr;
    };
    // The type decides how the other keys read, and enum options how the default does.
    const Entry* type = entry("type");
    if (type == nullptr) throw Fail{"missing 'type' (bool, int, float, enum, string or key)", table_line};
    at_line(type->line, [&] {
        const std::string text = get_string(*type->node, "type", false);
        if (text == "bool") s.type = SettingType::Bool;
        else if (text == "int") s.type = SettingType::Int;
        else if (text == "float") s.type = SettingType::Float;
        else if (text == "enum") s.type = SettingType::Enum;
        else if (text == "string") s.type = SettingType::String;
        else if (text == "key") s.type = SettingType::Key;
        else throw Fail{"type must be bool, int, float, enum, string or key, not \"" + text + "\""};
    });
    if (const Entry* values = entry("values")) {
        at_line(values->line, [&] {
            if (s.type != SettingType::Enum) throw Fail{"values apply only to enum settings"};
            s.options = parse_options(*values->node);
        });
    } else if (s.type == SettingType::Enum) {
        throw Fail{"an enum setting needs 'values'", table_line};
    }
    bool has_default = false, has_label = false;
    for (const auto& [key, item, line] : entries) {
        at_line(line, [&] {
            if (key == "default") {
                s.default_value = value_from_node(s, *item, "default");
                has_default = true;
            } else if (key == "min" || key == "max" || key == "step") {
                if (!is_numeric(s.type)) throw Fail{key + " applies only to int and float settings"};
                const SettingValue limit = value_from_node(s, *item, key);
                if (key == "step" && !(s.type == SettingType::Int ? limit.integer > 0 : limit.real > 0)) {
                    throw Fail{"step must be greater than 0"};
                }
                (key == "min" ? s.min : key == "max" ? s.max : s.step) = limit;
            } else if (key == "label") {
                s.label = get_string(*item, key, false);
                has_label = true;
            } else if (key == "description") {
                s.description = get_string(*item, key, true);
            } else if (key == "group") {
                s.group = get_string(*item, key, true);
            } else if (key == "restart" || key == "advanced" || key == "affects_gameplay") {
                const auto* b = item->as_boolean();
                if (b == nullptr) throw Fail{key + " must be true or false"};
                if (key == "restart") s.restart = b->get();
                else if (key == "advanced") s.advanced = b->get();
                else s.affects_gameplay = b->get();
            } else if (key == "module") {
                s.module = get_string(*item, key, false);
                s.module_line = line;
            }
        });
    }
    if (!has_default) throw Fail{"missing 'default'", table_line};
    if (!has_label) throw Fail{"missing 'label'", table_line};
    if (s.min && s.max && below(s, *s.max, *s.min)) {
        throw Fail{"max " + setting_toml(s, *s.max) + " is below min " + setting_toml(s, *s.min), entry("max")->line};
    }
    if (const std::string problem = check_setting_value(s, s.default_value); !problem.empty()) {
        throw Fail{"default: " + problem, entry("default")->line};
    }
    return s;
}

// A scalar of a retired setting's list: bool, int, float or string.
std::optional<RetiredValue> retired_value(const toml::node& node) {
    RetiredValue v;
    if (const auto* b = node.as_boolean()) {
        v.kind = RetiredValue::Kind::Bool;
        v.integer = b->get() ? 1 : 0;
    } else if (const auto* i = node.as_integer()) {
        v.kind = RetiredValue::Kind::Int;
        v.integer = i->get();
    } else if (const auto* f = node.as_floating_point()) {
        v.kind = RetiredValue::Kind::Float;
        v.real = f->get();
    } else if (const auto* s = node.as_string()) {
        v.kind = RetiredValue::Kind::String;
        v.text = s->get();
    } else {
        return std::nullopt;
    }
    return v;
}

// Two values of a retired list are the same: the same TOML type, except that
// an int and a float compare as numbers.
bool retired_same(const RetiredValue& a, const RetiredValue& b) {
    using Kind = RetiredValue::Kind;
    const auto number = [](const RetiredValue& x) { return x.kind == Kind::Int ? static_cast<double>(x.integer) : x.real; };
    const auto numeric = [](Kind k) { return k == Kind::Int || k == Kind::Float; };
    if (numeric(a.kind) && numeric(b.kind)) {
        return a.kind == Kind::Int && b.kind == Kind::Int ? a.integer == b.integer : number(a) == number(b);
    }
    if (a.kind != b.kind) return false;
    return a.kind == Kind::String ? a.text == b.text : a.integer == b.integer;
}

bool retired_in(const std::vector<RetiredValue>& list, const RetiredValue& v) {
    return std::any_of(list.begin(), list.end(), [&](const RetiredValue& o) { return retired_same(o, v); });
}

std::string retired_text(const RetiredValue& v) {
    switch (v.kind) {
        case RetiredValue::Kind::Bool: return v.integer != 0 ? "true" : "false";
        case RetiredValue::Kind::Int: return std::to_string(v.integer);
        case RetiredValue::Kind::Float: {
            std::ostringstream out;
            out << v.real;
            return out.str();
        }
        case RetiredValue::Kind::String: return "\"" + v.text + "\"";
    }
    return {};
}

bool retired_matches(const std::vector<RetiredValue>& list, const toml::node& node) {
    const auto v = retired_value(node);
    return v && retired_in(list, *v);
}

RetiredSetting parse_retired(const std::string& name, const toml::node& node, uint32_t table_line) {
    const auto* table = node.as_table();
    if (table == nullptr) throw Fail{"must be a table"};
    RetiredSetting r;
    r.name = name;
    bool has_module = false;
    for (const auto& [key, item, line] : in_file_order(*table)) {
        at_line(line, [&] {
            if (!kRetiredKeys.contains(key)) throw Fail{"unknown key '" + key + "'"};
            if (key == "module") {
                r.module = get_string(*item, key, false);
                r.module_line = line;
                has_module = true;
                return;
            }
            std::vector<RetiredValue>& list = key == "off" ? r.off : key == "on" ? r.on : key == "ironman" ? r.ironman : r.multiplayer;
            const auto* array = item->as_array();
            if (array == nullptr || array->empty()) {
                throw Fail{key + " must be a non-empty array of the values that meant it, such as [\"" +
                           (key == "off" ? std::string("off") : std::string("on")) + "\", " +
                           (key == "off" ? "false" : "true") + "]"};
            }
            for (const auto& value : *array) {
                auto v = retired_value(value);
                if (!v) throw Fail{"each of " + key + " must be a bool, an integer, a number or a string", line_of(value.source())};
                list.push_back(std::move(*v));
            }
        });
    }
    if (!has_module) throw Fail{"missing 'module' (the [[modules]] id whose switch replaces the setting)", table_line};
    for (const RetiredValue& v : r.on) {
        if (retired_in(r.off, v)) throw Fail{"a value cannot be in both off and on: " + retired_text(v), table_line};
    }
    return r;
}

// ---- key names ------------------------------------------------------------------

struct KeyName {
    const char* name;
    int code;
};

// Windows virtual-key codes. Letters, digits and F1-F24 are generated.
constexpr KeyName kKeys[] = {
    {"Backspace", 0x08}, {"Tab", 0x09},         {"Enter", 0x0D},        {"Pause", 0x13},
    {"CapsLock", 0x14},  {"Escape", 0x1B},      {"Space", 0x20},        {"PageUp", 0x21},
    {"PageDown", 0x22},  {"End", 0x23},         {"Home", 0x24},         {"Left", 0x25},
    {"Up", 0x26},        {"Right", 0x27},       {"Down", 0x28},         {"PrintScreen", 0x2C},
    {"Insert", 0x2D},    {"Delete", 0x2E},      {"Numpad0", 0x60},      {"Numpad1", 0x61},
    {"Numpad2", 0x62},   {"Numpad3", 0x63},     {"Numpad4", 0x64},      {"Numpad5", 0x65},
    {"Numpad6", 0x66},   {"Numpad7", 0x67},     {"Numpad8", 0x68},      {"Numpad9", 0x69},
    {"NumpadMultiply", 0x6A}, {"NumpadAdd", 0x6B}, {"NumpadSubtract", 0x6D}, {"NumpadDecimal", 0x6E},
    {"NumpadDivide", 0x6F}, {"NumLock", 0x90},  {"ScrollLock", 0x91},   {"Semicolon", 0xBA},
    {"Equals", 0xBB},    {"Comma", 0xBC},       {"Minus", 0xBD},        {"Period", 0xBE},
    {"Slash", 0xBF},     {"Backquote", 0xC0},   {"LeftBracket", 0xDB},  {"Backslash", 0xDC},
    {"RightBracket", 0xDD}, {"Quote", 0xDE},
};

std::optional<int> key_code(std::string_view name) {
    const std::string l = lower(name);
    if (l.size() == 1 && l[0] >= 'a' && l[0] <= 'z') return 0x41 + (l[0] - 'a');
    if (l.size() == 1 && l[0] >= '0' && l[0] <= '9') return 0x30 + (l[0] - '0');
    if (l.size() >= 2 && l.size() <= 3 && l[0] == 'f') {
        int n = 0;
        const auto [end, ec] = std::from_chars(l.data() + 1, l.data() + l.size(), n);
        if (ec == std::errc() && end == l.data() + l.size() && n >= 1 && n <= 24 && l[1] != '0') return 0x70 + n - 1;
    }
    for (const auto& k : kKeys) {
        if (lower(k.name) == l) return k.code;
    }
    return std::nullopt;
}

std::string code_name(int code) {
    if (code >= 0x41 && code <= 0x5A) return std::string(1, static_cast<char>(code));
    if (code >= 0x30 && code <= 0x39) return std::string(1, static_cast<char>(code));
    if (code >= 0x70 && code <= 0x87) return "F" + std::to_string(code - 0x70 + 1);
    for (const auto& k : kKeys) {
        if (k.code == code) return k.name;
    }
    return {};
}

}  // namespace

const char* setting_type_name(SettingType type) {
    switch (type) {
    case SettingType::Bool: return "bool";
    case SettingType::Int: return "int";
    case SettingType::Float: return "float";
    case SettingType::Enum: return "enum";
    case SettingType::String: return "string";
    case SettingType::Key: return "key";
    }
    return "?";
}

const Setting* SettingsFile::find(std::string_view name) const {
    for (const auto& s : settings) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

const RetiredSetting* SettingsFile::find_retired(std::string_view name) const {
    for (const auto& r : retired) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

std::string settings_module_error(const SettingsFile& file, const Manifest& manifest, uint32_t& line) {
    const auto declared = [&](const std::string& id) {
        return std::any_of(manifest.modules.begin(), manifest.modules.end(), [&](const Module& m) { return m.id == id; });
    };
    const auto problem = [&](const std::string& what, const std::string& id) {
        return "settings.toml: " + what + ": module '" + id + "' is not declared in plugin.toml" +
               (manifest.modules.empty() ? std::string(", which has no [[modules]]") : std::string(" [[modules]]"));
    };
    for (const auto& s : file.settings) {
        if (!s.module.empty() && !declared(s.module)) {
            line = s.module_line;
            return problem("setting " + s.name, s.module);
        }
    }
    for (const auto& r : file.retired) {
        const auto m = std::find_if(manifest.modules.begin(), manifest.modules.end(), [&](const Module& x) { return x.id == r.module; });
        if (m == manifest.modules.end()) {
            line = r.module_line;
            return problem("retired " + r.name, r.module);
        }
        for (const auto& [list, key, offered] : {std::tuple{&r.ironman, "ironman", m->allow_ironman == Allow::OptIn},
                                                 std::tuple{&r.multiplayer, "multiplayer", m->allow_multiplayer == Allow::OptIn}}) {
            if (!list->empty() && !offered) {
                line = r.line;
                return "settings.toml: retired " + r.name + ": " + key + " needs module '" + r.module +
                       "' to offer allow_" + key + " = \"opt-in\" in plugin.toml";
            }
        }
    }
    return {};
}

bool setting_counts(const Setting& setting, const Manifest& manifest) {
    bool owner_affects = manifest.affects_gameplay;
    if (!setting.module.empty()) {
        owner_affects = false;
        for (const Module& m : manifest.modules) {
            if (m.id == setting.module) owner_affects = m.affects_gameplay;
        }
    }
    return owner_affects && setting.affects_gameplay.value_or(setting.type != SettingType::Key);
}

std::optional<int64_t> parse_key(std::string_view text) {
    if (text.empty()) return 0;
    int64_t modifiers = 0;
    while (true) {
        const size_t plus = text.find('+');
        const std::string_view part = text.substr(0, plus);
        if (plus == std::string_view::npos) {
            const auto code = key_code(part);
            if (!code) return std::nullopt;
            return modifiers | *code;
        }
        const std::string l = lower(part);
        const int64_t modifier = l == "ctrl" || l == "control" ? kKeyCtrl
                                 : l == "shift"                ? kKeyShift
                                 : l == "alt"                  ? kKeyAlt
                                                               : 0;
        if (modifier == 0 || (modifiers & modifier) != 0) return std::nullopt;
        modifiers |= modifier;
        text.remove_prefix(plus + 1);
        if (text.empty()) return std::nullopt;
    }
}

std::string key_name(int64_t code) {
    if (code <= 0 || (code & ~(kKeyShift | kKeyCtrl | kKeyAlt | 0xFF)) != 0) return {};
    const std::string key = code_name(static_cast<int>(code & 0xFF));
    if (key.empty()) return {};
    std::string out;
    if (code & kKeyCtrl) out += "Ctrl+";
    if (code & kKeyShift) out += "Shift+";
    if (code & kKeyAlt) out += "Alt+";
    return out + key;
}

std::string check_setting_value(const Setting& s, const SettingValue& v) {
    switch (s.type) {
    case SettingType::Bool:
        return v.integer == 0 || v.integer == 1 ? std::string() : "a bool is 0 or 1";
    case SettingType::Int:
    case SettingType::Float:
        if (s.type == SettingType::Float && !std::isfinite(v.real)) return "not a finite number";
        if (s.min && below(s, v, *s.min)) return setting_toml(s, v) + " is below the minimum " + setting_toml(s, *s.min);
        if (s.max && below(s, *s.max, v)) return setting_toml(s, v) + " is above the maximum " + setting_toml(s, *s.max);
        return {};
    case SettingType::Enum:
        return v.integer >= 0 && static_cast<size_t>(v.integer) < s.options.size()
                   ? std::string()
                   : "option " + std::to_string(v.integer) + " does not exist";
    case SettingType::String:
        return v.text.find('\0') == std::string::npos ? std::string() : "a NUL character";
    case SettingType::Key:
        return v.integer == 0 || !key_name(v.integer).empty() ? std::string()
                                                              : "key code " + std::to_string(v.integer) + " is not a key binding";
    }
    return "unknown type";
}

std::optional<SettingValue> parse_setting_text(const Setting& s, std::string_view text, std::string& error) {
    SettingValue v;
    switch (s.type) {
    case SettingType::Bool: {
        const std::string l = lower(text);
        if (l == "true" || l == "1") v.integer = 1;
        else if (l == "false" || l == "0") v.integer = 0;
        else error = "\"" + std::string(text) + "\" is not true or false";
        break;
    }
    case SettingType::Int: {
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v.integer);
        if (text.empty() || ec != std::errc() || end != text.data() + text.size()) {
            error = "\"" + std::string(text) + "\" is not an integer";
        }
        break;
    }
    case SettingType::Float: {
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v.real);
        if (text.empty() || ec != std::errc() || end != text.data() + text.size()) {
            error = "\"" + std::string(text) + "\" is not a number";
        }
        break;
    }
    case SettingType::Enum: {
        const auto it = std::find_if(s.options.begin(), s.options.end(), [&](const auto& o) { return o.name == text; });
        if (it == s.options.end()) error = "\"" + std::string(text) + "\" is not one of " + option_list(s);
        else v.integer = it - s.options.begin();
        break;
    }
    case SettingType::String:
        v.text = text;
        break;
    case SettingType::Key: {
        const auto code = parse_key(text);
        if (!code) error = "\"" + std::string(text) + "\" is not a key binding such as \"Ctrl+Shift+F12\"";
        else v.integer = *code;
        break;
    }
    }
    if (error.empty()) error = check_setting_value(s, v);
    if (!error.empty()) return std::nullopt;
    return v;
}

std::string setting_toml(const Setting& s, const SettingValue& v) {
    switch (s.type) {
    case SettingType::Bool: return v.integer != 0 ? "true" : "false";
    case SettingType::Int: return std::to_string(v.integer);
    case SettingType::Float: return format_float(v.real);
    case SettingType::Enum:
        return v.integer >= 0 && static_cast<size_t>(v.integer) < s.options.size()
                   ? toml_string(s.options[static_cast<size_t>(v.integer)].name)
                   : std::to_string(v.integer);
    case SettingType::String: return toml_string(v.text);
    case SettingType::Key: return toml_string(key_name(v.integer));
    }
    return {};
}

SettingsFileResult parse_settings_text(std::string_view text) {
    SettingsFileResult result;
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::ostringstream message;
        message << "settings.toml: " << e.description() << " (line " << e.source().begin.line << ")";
        result.error = message.str();
        result.line = line_of(e.source());
        return result;
    }
    SettingsFile file;
    std::string where;
    try {
        for (const auto& [key, node, line] : in_file_order(table)) {
            if (!kTopKeys.contains(key)) throw Fail{"unknown key '" + key + "'", line};
        }
        if (const auto* node = table.get("setting")) {
            const auto* settings = node->as_table();
            if (settings == nullptr) {
                throw Fail{"setting must be a table of [setting.<name>] tables", line_of(node->source())};
            }
            for (const auto& [name, item, line] : in_file_order(*settings)) {
                where = "setting " + name + ": ";
                Setting s;
                at_line(line, [&] {
                    if (!valid_name(name)) throw Fail{"the name must match [a-z][a-z0-9_]* (max 64 characters)"};
                    s = parse_setting(name, *item, line);
                });
                s.id = static_cast<uint32_t>(file.settings.size());
                s.line = line;
                file.settings.push_back(std::move(s));
            }
        }
        where.clear();
        if (const auto* node = table.get("retired")) {
            const auto* retired = node->as_table();
            if (retired == nullptr) {
                throw Fail{"retired must be a table of [retired.<name>] tables", line_of(node->source())};
            }
            for (const auto& [name, item, line] : in_file_order(*retired)) {
                where = "retired " + name + ": ";
                RetiredSetting r;
                at_line(line, [&] {
                    if (!valid_name(name)) throw Fail{"the name must match [a-z][a-z0-9_]* (max 64 characters)"};
                    if (file.find(name) != nullptr) {
                        throw Fail{"a setting of this name is declared; a retired setting is one the plugin no longer has"};
                    }
                    r = parse_retired(name, *item, line);
                });
                r.line = line;
                file.retired.push_back(std::move(r));
            }
        }
    } catch (const Fail& f) {
        result.error = "settings.toml: " + where + f.message;
        result.line = f.line;
        return result;
    }
    result.file = std::move(file);
    return result;
}

namespace {

std::string shown(const std::filesystem::path& path) {
    const auto u8 = path.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

bool read_text(const std::filesystem::path& file, std::string& text) {
    std::error_code size_error;
    if (const std::uintmax_t size = std::filesystem::file_size(file, size_error);
        !size_error && size > kMaxPluginTomlBytes) {
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);  // saved by Notepad
    return true;
}

}  // namespace

SettingsFileResult load_settings(const std::filesystem::path& file) {
    std::string text;
    if (!read_text(file, text)) {
        SettingsFileResult result;
        result.error = "cannot read " + shown(file.filename());
        return result;
    }
    return parse_settings_text(text);
}

PlayerSettings parse_player_settings(const SettingsFile& schema, std::string_view text) {
    PlayerSettings out;
    out.values.resize(schema.settings.size());
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::ostringstream message;
        message << "line " << e.source().begin.line << ": " << e.description()
                << "; the file is ignored and every setting has its default";
        out.warnings.push_back(message.str());
        return out;
    }
    for (const auto& [name, node, line] : in_file_order(table)) {
        const std::string at = "line " + std::to_string(line) + ": ";
        const Setting* s = schema.find(name);
        if (s == nullptr) {
            out.warnings.push_back(at + "'" + name + "' is not a setting of this plugin; ignored");
            continue;
        }
        const std::string fallback = "; the default " + setting_toml(*s, s->default_value) + " applies";
        try {
            const SettingValue v = value_from_node(*s, *node, name);
            if (const std::string problem = check_setting_value(*s, v); !problem.empty()) {
                out.warnings.push_back(at + name + ": " + problem + fallback);
                continue;
            }
            out.values[s->id] = v;
        } catch (const Fail& f) {
            out.warnings.push_back(at + f.message + fallback);
        }
    }
    return out;
}

PlayerSettings load_player_settings(const SettingsFile& schema, const std::filesystem::path& file) {
    std::string text;
    if (!read_text(file, text)) {
        PlayerSettings out;
        out.values.resize(schema.settings.size());
        return out;
    }
    return parse_player_settings(schema, text);
}

std::string player_settings_text(const SettingsFile& schema, const std::vector<SettingValue>& values) {
    std::string body;
    for (const auto& s : schema.settings) {
        if (s.id >= values.size() || values[s.id] == s.default_value) continue;
        body += s.name + " = " + setting_toml(s, values[s.id]) + "\n";
    }
    if (body.empty()) return {};
    return "# The player's values of this plugin's settings that differ from the defaults (settings.toml in\n"
           "# the plugin folder). Anvil rewrites this file when a value changes; edit it while the game is closed.\n" +
           body;
}

bool write_player_settings(const SettingsFile& schema, const std::vector<SettingValue>& values,
                           const std::filesystem::path& file, std::string& error) {
    return replace_text_file(file, player_settings_text(schema, values), error);
}

namespace {

// A value of the player's file as TOML wrote it, for the log.
std::string node_text(const toml::node& node) {
    if (const auto* b = node.as_boolean()) return b->get() ? "true" : "false";
    if (const auto* i = node.as_integer()) return std::to_string(i->get());
    if (const auto* f = node.as_floating_point()) return format_float(f->get());
    if (const auto* s = node.as_string()) return toml_string(s->get());
    return node.is_array() ? "[...]" : "{...}";
}

bool only_comments(std::string_view text) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(start, end - start);
        const size_t first = line.find_first_not_of(" \t\r");
        if (first != std::string_view::npos && line[first] != '#') return false;
        start = end + 1;
    }
    return true;
}

}  // namespace

std::optional<RetiredMove> plan_retired_move(const SettingsFile& schema, std::string_view text, std::string& error) {
    RetiredMove move;
    if (schema.retired.empty()) return move;
    toml::table table;
    try {
        table = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::ostringstream message;
        message << "line " << e.source().begin.line << ": " << e.description();
        error = message.str();
        return std::nullopt;
    }
    std::vector<std::pair<uint32_t, uint32_t>> drop;  // 1-based lines, inclusive
    toml::table expected = table;
    for (const auto& [name, node, line] : in_file_order(table)) {
        const RetiredSetting* r = schema.find_retired(name);
        if (r == nullptr) continue;
        if (const auto* t = node->as_table(); t != nullptr && !t->is_inline()) {
            error = "line " + std::to_string(line) + ": '" + name + "' is a table, not a value";
            return std::nullopt;
        }
        RetiredMove::Item item;
        item.name = name;
        item.module = r->module;
        item.value = node_text(*node);
        item.line = line;
        item.turns_off = retired_matches(r->off, *node);
        item.turns_on = retired_matches(r->on, *node);
        item.allows_ironman = retired_matches(r->ironman, *node);
        item.allows_multiplayer = retired_matches(r->multiplayer, *node);
        move.items.push_back(std::move(item));
        drop.emplace_back(line, std::max(line, static_cast<uint32_t>(node->source().end.line)));
        expected.erase(name);
    }
    if (move.items.empty()) return move;
    std::string out;
    uint32_t number = 1;
    for (size_t start = 0; start < text.size(); ++number) {
        size_t end = text.find('\n', start);
        end = end == std::string_view::npos ? text.size() : end + 1;
        const bool dropped = std::any_of(drop.begin(), drop.end(), [&](const auto& d) { return number >= d.first && number <= d.second; });
        if (!dropped) out.append(text.substr(start, end - start));
        start = end;
    }
    try {
        if (toml::parse(out) != expected) {
            error = "the file would not read back as itself without the retired values";
            return std::nullopt;
        }
    } catch (const toml::parse_error&) {
        error = "the file would not parse without the retired values";
        return std::nullopt;
    }
    move.text = only_comments(out) ? std::string() : std::move(out);
    return move;
}

bool replace_text_file(const std::filesystem::path& file, const std::string& text, std::string& error) {
    std::error_code ec;
    if (text.empty()) {
        std::filesystem::remove(file, ec);
        if (ec) error = "cannot remove " + shown(file) + ": " + ec.message();
        return !ec;
    }
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path temporary = file;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        if (!out) {
            error = "cannot write " + shown(temporary);
            return false;
        }
    }
    std::filesystem::rename(temporary, file, ec);  // replaces the file in one step
    if (ec) {
        error = "cannot replace " + shown(file) + ": " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

std::string setting_environment_name(std::string_view plugin_id, std::string_view setting_name) {
    std::string out = "ANVIL_";
    for (const std::string_view part : {plugin_id, std::string_view("_"), setting_name}) {
        for (const char c : part) {
            out.push_back(c == '-' ? '_' : c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
        }
    }
    return out;
}

}  // namespace anvil
