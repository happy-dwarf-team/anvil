#include "files.hpp"

#include <anvil/anvil_map.h>

#include "modes.hpp"
#include "text.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace anvil_map::files {

namespace {

// Windows-1252's 0x80..0x9F; 0 where it has no character.
constexpr char16_t kHigh1252[32] = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
                                    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// The file's text as UTF-8: unchanged when it is UTF-8 (a BOM dropped), else
// read as Windows-1252 with '?' for its five unused bytes.
std::string as_utf8(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    if (valid_utf8(text)) return std::string(text);
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (const char ch : text) {
        const auto b = static_cast<unsigned char>(ch);
        if (b < 0x80) {
            out += ch;
        } else if (b < 0xA0) {
            const char16_t c = kHigh1252[b - 0x80];
            append_utf8(out, c != 0 ? c : U'?');
        } else {
            append_utf8(out, b);
        }
    }
    return out;
}

struct Fault {
    int line;
    std::string what;
};

// The key a test is written with, for faults about where it reads.
std::string key_of(Test test) {
    switch (test) {
        case Test::owned_by: return "owned_by";
        case Test::controlled_by: return "controlled_by";
        case Test::core:
        case Test::core_of: return "core";
        case Test::occupied: return "occupied";
        case Test::subject: return "subject";
        case Test::subject_of: return "subject_of";
        case Test::trade_goods: return "trade_goods";
        default: return "this test";
    }
}

struct Token {
    enum Kind { word, string, open, close, equals, end } kind = end;
    std::string text;
    int line = 0;
};

class Lexer {
public:
    explicit Lexer(std::string_view text) : text_(text) {}

    Token next() {
        skip();
        Token t;
        t.line = line_;
        if (at_ >= text_.size()) return t;
        const char c = text_[at_];
        if (c == '{') {
            ++at_;
            ++depth_;
            t.kind = Token::open;
        } else if (c == '}') {
            ++at_;
            if (depth_ > 0) --depth_;
            t.kind = Token::close;
        } else if (c == '=') {
            ++at_;
            t.kind = Token::equals;
        } else if (c == '<' || c == '>' || c == '!') {
            throw Fault{line_, "comparison operators are not read here: write at_least, at_most, above or below"};
        } else if (c == '"') {
            const std::size_t end = text_.find_first_of("\"\n", at_ + 1);
            if (end == std::string_view::npos || text_[end] != '"') throw Fault{line_, "a quoted text has no end"};
            t.kind = Token::string;
            t.text = std::string(text_.substr(at_ + 1, end - at_ - 1));
            at_ = end + 1;
        } else {
            const std::size_t start = at_;
            while (at_ < text_.size() && !std::isspace(static_cast<unsigned char>(text_[at_])) &&
                   std::string_view("{}=\"#<>!").find(text_[at_]) == std::string_view::npos) {
                ++at_;
            }
            t.kind = Token::word;
            t.text = std::string(text_.substr(start, at_ - start));
        }
        return t;
    }

    int depth() const { return depth_; }
    int line() const { return line_; }

    // After a fault: on to the end of the block at depth 0 the fault is in.
    void recover() {
        while (depth_ > 0) {
            skip();
            if (at_ >= text_.size()) return;
            const char c = text_[at_];
            if (c == '"') {
                const std::size_t end = text_.find_first_of("\"\n", at_ + 1);
                at_ = end == std::string_view::npos ? text_.size() : end + 1;
                continue;
            }
            if (c == '{') ++depth_;
            if (c == '}') --depth_;
            ++at_;
        }
    }

private:
    void skip() {
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if (c == '\n') {
                ++line_;
                ++at_;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                ++at_;
            } else if (c == '#') {
                while (at_ < text_.size() && text_[at_] != '\n') ++at_;
            } else {
                return;
            }
        }
    }

    std::string_view text_;
    std::size_t at_ = 0;
    int line_ = 1;
    int depth_ = 0;
};

struct ValueName {
    std::string_view name;
    Value value;
};
constexpr ValueName kValues[] = {
    {"base_tax", Value::base_tax},       {"base_production", Value::base_production},
    {"base_manpower", Value::base_manpower}, {"development", Value::development},
    {"treasury", Value::treasury},       {"stability", Value::stability},
    {"prestige", Value::prestige},       {"manpower", Value::manpower},
    {"monthly_income", Value::monthly_income}, {"provinces", Value::provinces},
};

bool name_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }
bool sprite_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

class Parser {
public:
    explicit Parser(Lexer& lexer) : lex_(lexer) {}

    Token expect(Token::Kind kind, const char* what) {
        Token t = lex_.next();
        if (t.kind != kind) throw Fault{t.line, std::string("expected ") + what};
        return t;
    }

    // A text: quoted, or one word.
    std::string text(const char* key) {
        Token t = lex_.next();
        if (t.kind != Token::string && t.kind != Token::word) throw Fault{t.line, std::string(key) + " needs a text"};
        return t.text;
    }

    // A country tag: three letters or digits, upper-cased.
    static bool is_tag(const std::string& v) {
        return v.size() == 3 && std::all_of(v.begin(), v.end(), [](char c) {
                   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
               });
    }
    static std::string upper(std::string v) {
        for (char& c : v) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        return v;
    }

    bool yes_no(const char* key) {
        const Token t = lex_.next();
        if (t.kind == Token::word && t.text == "yes") return true;
        if (t.kind == Token::word && t.text == "no") return false;
        throw Fault{t.line, std::string(key) + " is yes or no"};
    }

    // A decimal number with at most three decimals, x 1000.
    std::int64_t number(const char* key) {
        const Token t = lex_.next();
        const std::string& s = t.text;
        std::size_t i = 0;
        const bool negative = !s.empty() && s[0] == '-';
        if (negative) i = 1;
        std::int64_t whole = 0, fraction = 0;
        int digits = 0, decimals = 0;
        bool point = false;
        bool ok = t.kind == Token::word && i < s.size();
        for (; ok && i < s.size(); ++i) {
            const char c = s[i];
            if (c == '.' && !point) {
                point = true;
            } else if (c >= '0' && c <= '9') {
                if (point) {
                    if (++decimals > 3) ok = false;
                    fraction = fraction * 10 + (c - '0');
                } else {
                    if (++digits > 12) ok = false;
                    whole = whole * 10 + (c - '0');
                }
            } else {
                ok = false;
            }
        }
        if (!ok || digits + decimals == 0) {
            throw Fault{t.line, std::string(key) + " needs a number with at most three decimals"};
        }
        for (; decimals < 3; ++decimals) fraction *= 10;
        const std::int64_t value = whole * 1000 + fraction;
        return negative ? -value : value;
    }

    std::uint32_t color() {
        expect(Token::open, "{ r g b } after color =");
        std::uint32_t out = 0;
        for (int i = 0; i < 3; ++i) {
            const Token t = lex_.next();
            int v = -1;
            if (t.kind == Token::word && !t.text.empty() && t.text.size() <= 3 &&
                std::all_of(t.text.begin(), t.text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                v = std::stoi(t.text);
            }
            if (v < 0 || v > 255) throw Fault{t.line, "a colour is three whole numbers 0 to 255: color = { r g b }"};
            out = (out << 8) | static_cast<std::uint32_t>(v);
        }
        expect(Token::close, "} after the three numbers of a colour");
        return out;
    }

    Rule rule(int line) {
        expect(Token::open, "{ after rule =");
        Rule r;
        r.line = line;
        int tests = 0;
        bool has_color = false;
        bool has_label = false;
        bool has_owner = false;
        bool has_high_color = false;
        for (;;) {
            const Token key = lex_.next();
            if (key.kind == Token::close) break;
            if (key.kind != Token::word) throw Fault{key.line, "expected a key or } in a rule"};
            expect(Token::equals, ("= after " + key.text).c_str());
            const std::string& k = key.text;
            if (k == "always") {
                if (!yes_no("always")) throw Fault{key.line, "always = no never passes: leave the rule out"};
                r.test = Test::always;
                ++tests;
            } else if (k == "value") {
                r.test = Test::value;
                r.name = text("value");
                const auto* found = std::find_if(std::begin(kValues), std::end(kValues),
                                                 [&](const ValueName& v) { return v.name == r.name; });
                if (found == std::end(kValues)) {
                    throw Fault{key.line, "value is one of development, base_tax, base_production, base_manpower, and "
                                          "with owner = yes treasury, stability, prestige, manpower, monthly_income, "
                                          "provinces: " + r.name};
                }
                r.value = found->value;
                ++tests;
            } else if (k == "owned_by" || k == "controlled_by" || k == "subject_of") {
                r.test = k == "owned_by" ? Test::owned_by : k == "controlled_by" ? Test::controlled_by : Test::subject_of;
                r.name = text(k.c_str());
                if (!is_tag(r.name)) throw Fault{key.line, k + " needs a country tag of three letters or digits: " + r.name};
                r.name = upper(r.name);
                ++tests;
            } else if (k == "core") {
                // core = yes / no: the owner's core; core = TAG: that country's.
                const std::string v = text("core");
                if (v == "yes" || v == "no") {
                    r.test = Test::core;
                    r.name = k;
                    r.yes = v == "yes";
                } else if (is_tag(v)) {
                    r.test = Test::core_of;
                    r.name = upper(v);
                } else {
                    throw Fault{key.line, "core is yes, no or a country tag of three letters or digits: " + v};
                }
                ++tests;
            } else if (k == "religion" || k == "culture" || k == "trade_goods") {
                r.test = k == "religion" ? Test::religion : k == "culture" ? Test::culture : Test::trade_goods;
                r.name = text(k.c_str());
                const bool key_like = !r.name.empty() && r.name.size() <= 128 &&
                                      std::all_of(r.name.begin(), r.name.end(), [](char ch) {
                                          return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                                 (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
                                      });
                if (!key_like) {
                    throw Fault{key.line, k + " needs a script key as the game's files write it (orthodox, russian, "
                                              "grain): " + r.name};
                }
                ++tests;
            } else if (k == "occupied" || k == "subject") {
                r.test = k == "occupied" ? Test::occupied : Test::subject;
                r.name = k;
                r.yes = yes_no(k.c_str());
                ++tests;
            } else if (k == "flag" || k == "variable" || k == "modifier") {
                r.test = k == "flag" ? Test::flag : k == "variable" ? Test::variable : Test::modifier;
                r.name = text(k.c_str());
                if (r.name.empty() || r.name.size() > 128) throw Fault{key.line, k + " needs a name of 1 to 128 bytes"};
                ++tests;
            } else if (k == "at_least" || k == "at_most" || k == "above" || k == "below") {
                std::optional<std::int64_t>& bound =
                    k == "at_least" ? r.at_least : k == "at_most" ? r.at_most : k == "above" ? r.above : r.below;
                if (bound) throw Fault{key.line, k + " is given twice"};
                bound = number(k.c_str());
            } else if (k == "owner") {
                if (has_owner) throw Fault{key.line, "owner is given twice"};
                r.owner = yes_no("owner");
                has_owner = true;
            } else if (k == "scale") {
                if (r.low) throw Fault{key.line, "scale is given twice"};
                expect(Token::open, "{ low high } after scale =");
                r.low = number("scale");
                r.high = number("scale");
                expect(Token::close, "} after the two numbers of a scale");
                if (*r.low >= *r.high) throw Fault{key.line, "scale = { low high } needs low below high"};
            } else if (k == "high_color") {
                if (has_high_color) throw Fault{key.line, "high_color is given twice"};
                r.high_color = color();
                has_high_color = true;
            } else if (k == "color") {
                if (has_color) throw Fault{key.line, "color is given twice"};
                r.color = color();
                has_color = true;
            } else if (k == "label") {
                if (has_label) throw Fault{key.line, "label is given twice"};
                r.label = text("label");
                has_label = true;
            } else {
                throw Fault{key.line, "a rule has no key " + k};
            }
        }
        if (tests != 1) {
            throw Fault{line, "a rule needs one of always, flag, variable, modifier, value, owned_by, controlled_by, "
                              "core, occupied, subject, subject_of, religion, culture or trade_goods"};
        }
        if (!has_color) throw Fault{line, "a rule needs color = { r g b }"};
        if (!has_label || r.label.empty() || r.label.size() > kLabelMax) {
            throw Fault{line, "a rule needs a label of 1 to 64 bytes"};
        }
        const bool bounded = r.at_least || r.at_most || r.above || r.below;
        if (bounded && (r.test == Test::always || is_yes_no(r.test))) {
            throw Fault{line, "at_least, at_most, above and below go with a variable, a modifier or a value"};
        }
        if (r.low.has_value() != has_high_color) {
            throw Fault{line, "scale = { low high } and high_color = { r g b } go together"};
        }
        if (r.low && (r.test == Test::always || is_yes_no(r.test))) {
            throw Fault{line, "a scale goes with a variable, a modifier or a value"};
        }
        if (r.test == Test::value && is_country_value(r.value) != r.owner) {
            throw Fault{line, r.owner ? r.name + " is the province's: leave out owner = yes"
                                      : r.name + " is the owner's: write owner = yes"};
        }
        if ((r.test == Test::subject || r.test == Test::subject_of) && !r.owner) {
            throw Fault{line, key_of(r.test) + " is the owner's: write owner = yes"};
        }
        if ((r.test == Test::owned_by || r.test == Test::controlled_by || r.test == Test::core ||
             r.test == Test::core_of || r.test == Test::occupied || r.test == Test::trade_goods) &&
            r.owner) {
            throw Fault{line, key_of(r.test) + " is the province's: leave out owner = yes"};
        }
        return r;
    }

    FileMode mode(const Token& name) {
        FileMode m;
        m.name = name.text;
        m.line = name.line;
        expect(Token::equals, ("= after " + m.name).c_str());
        expect(Token::open, ("{ after " + m.name + " =").c_str());
        // Checked inside its block, so a fault skips the whole mode.
        if (m.name.empty() || m.name.size() > kNameMax || !std::all_of(m.name.begin(), m.name.end(), name_char)) {
            throw Fault{name.line, "a mode's name is [a-z0-9_-], 1 to 32 characters: " + m.name};
        }
        std::set<std::string> seen;
        for (;;) {
            const Token key = lex_.next();
            if (key.kind == Token::close) break;
            if (key.kind != Token::word) throw Fault{key.line, "expected a key or } in " + m.name};
            expect(Token::equals, ("= after " + key.text).c_str());
            const std::string& k = key.text;
            if (k != "rule" && !seen.insert(k).second) throw Fault{key.line, k + " is given twice"};
            if (k == "label") {
                m.label = text("label");
            } else if (k == "description") {
                m.description = text("description");
            } else if (k == "sprite") {
                m.sprite = text("sprite");
                if (m.sprite.empty() || m.sprite.size() > kSpriteMax ||
                    !std::all_of(m.sprite.begin(), m.sprite.end(), sprite_char)) {
                    throw Fault{key.line, "a sprite is a name of [A-Za-z0-9_], such as GFX_mapmode_religion"};
                }
            } else if (k == "tab") {
                const std::int64_t tab = number("tab");
                if (tab % 1000 != 0 || tab < 1000 || tab > 4000) throw Fault{key.line, "tab is 1, 2, 3 or 4"};
                m.tab = static_cast<std::uint32_t>(tab / 1000);
            } else if (k == "legend") {
                m.legend = yes_no("legend");
            } else if (k == "rule") {
                if (m.rules.size() == kRulesMax) throw Fault{key.line, "a mode has at most 64 rules"};
                m.rules.push_back(rule(key.line));
            } else {
                throw Fault{key.line, "a mode has no key " + k};
            }
        }
        if (m.label.empty() || m.label.size() > kLabelMax) throw Fault{m.line, m.name + " needs a label of 1 to 64 bytes"};
        if (m.description.size() > kDescriptionMax) throw Fault{m.line, "the description is longer than 512 bytes"};
        if (m.rules.empty()) throw Fault{m.line, m.name + " has no rule"};
        return m;
    }

private:
    Lexer& lex_;
};

std::string read_file(const std::filesystem::path& path, bool& too_big) {
    too_big = false;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return {};
    if (size > kFileMax) {
        too_big = true;
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::filesystem::path utf8_path(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string path_utf8(const std::filesystem::path& path) {
    const std::u8string s = path.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

void parse(std::string_view raw, std::string_view source, std::vector<FileMode>& out,
           std::vector<std::string>& problems) {
    const std::string text = as_utf8(raw);
    Lexer lex(text);
    Parser parser(lex);
    for (;;) {
        try {
            const Token name = lex.next();
            if (name.kind == Token::end) return;
            if (name.kind != Token::word) throw Fault{name.line, "expected the name of a mode"};
            FileMode m = parser.mode(name);
            m.source = std::string(source);
            out.push_back(std::move(m));
        } catch (const Fault& f) {
            problems.push_back(std::string(source) + ":" + std::to_string(f.line) + ": " + f.what);
            lex.recover();
        }
    }
}

bool in_bounds(const Rule& rule, std::int64_t value) {
    if (!rule.at_least && !rule.at_most && !rule.above && !rule.below) return value != 0;
    return (!rule.at_least || value >= *rule.at_least) && (!rule.at_most || value <= *rule.at_most) &&
           (!rule.above || value > *rule.above) && (!rule.below || value < *rule.below);
}

bool is_country_value(Value value) {
    return value != Value::base_tax && value != Value::base_production && value != Value::base_manpower &&
           value != Value::development;
}

std::uint32_t color_at(const Rule& rule, std::int64_t value) {
    if (!rule.scaled()) return rule.color;
    const double span = static_cast<double>(*rule.high - *rule.low);
    const double at = std::clamp(static_cast<double>(value - *rule.low) / span, 0.0, 1.0);
    std::uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const double from = static_cast<double>((rule.color >> shift) & 0xFFu);
        const double to = static_cast<double>((rule.high_color >> shift) & 0xFFu);
        const auto channel = static_cast<std::uint32_t>(std::lround(from + (to - from) * at));
        out |= std::min<std::uint32_t>(channel, 255u) << shift;
    }
    return out;
}

std::string decimal(std::int64_t value) {
    const bool negative = value < 0;
    const std::uint64_t v = negative ? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
    std::string out = (negative ? "-" : "") + std::to_string(v / 1000);
    if (const std::uint64_t f = v % 1000; f != 0) {
        std::string digits = std::to_string(1000 + f).substr(1);
        while (digits.back() == '0') digits.pop_back();
        out += "." + digits;
    }
    return out;
}

bool is_yes_no(Test test) {
    return test == Test::flag || test == Test::owned_by || test == Test::core || test == Test::occupied ||
           test == Test::subject || test == Test::controlled_by || test == Test::core_of || test == Test::subject_of ||
           test == Test::religion || test == Test::culture || test == Test::trade_goods;
}

Evaluator::Evaluator(const FileMode& mode, World& world) : mode_(mode), world_(world) {
    ids_.reserve(mode.rules.size());
    for (const Rule& r : mode.rules) {
        std::optional<std::int32_t> id;
        if (r.test == Test::flag) id = world.flag_id(r.name);
        if (r.test == Test::modifier) {
            id = world.modifier_id(r.name);
            if (!id) unknown_.push_back(r.name);
        }
        ids_.push_back(id);
    }
}

std::optional<std::int64_t> Evaluator::read_country(std::size_t i, const std::string& tag) {
    CountryReads& reads = countries_[tag];
    if (reads.done.empty()) {
        reads.value.resize(mode_.rules.size());
        reads.done.resize(mode_.rules.size());
    }
    if (reads.done[i]) return reads.value[i];
    const Rule& r = mode_.rules[i];
    std::optional<std::int64_t> v;
    switch (r.test) {
        case Test::always:
            v = 1;
            break;
        case Test::flag:
            if (ids_[i]) v = world_.country_has_flag(tag, *ids_[i]) ? 1 : 0;
            break;
        case Test::variable:
            v = world_.country_variable(tag, r.name);
            break;
        case Test::modifier:
            if (ids_[i]) v = world_.country_modifier(tag, *ids_[i]);
            break;
        case Test::value:
            v = world_.country_value(tag, r.value);
            break;
        case Test::subject:
            v = world_.overlord(tag).empty() != r.yes ? 1 : 0;
            break;
        case Test::subject_of:
            v = world_.overlord(tag) == r.name ? 1 : 0;
            break;
        case Test::religion:
        case Test::culture: {
            const std::string key = world_.country_key(tag, r.test);
            if (!key.empty()) v = key == r.name ? 1 : 0;
            break;
        }
        case Test::trade_goods:
        case Test::owned_by:
        case Test::controlled_by:
        case Test::core:
        case Test::core_of:
        case Test::occupied:
            break;  // the province's: refused with owner = yes
    }
    reads.done[i] = true;
    reads.value[i] = v;
    return v;
}

std::optional<std::int64_t> Evaluator::read(std::size_t i, std::int32_t province, std::optional<std::string>& owner) {
    const Rule& r = mode_.rules[i];
    if (r.owner) {
        if (!owner) owner = world_.owner(province);
        if (owner->empty()) return std::nullopt;
        return read_country(i, *owner);
    }
    switch (r.test) {
        case Test::always:
            return 1;
        case Test::flag:
            if (!ids_[i]) return std::nullopt;
            return world_.has_flag(province, *ids_[i]) ? 1 : 0;
        case Test::variable:
            return world_.variable(province, r.name);
        case Test::modifier:
            if (!ids_[i]) return std::nullopt;
            return world_.modifier(province, *ids_[i]);
        case Test::value:
            return world_.province_value(province, r.value);
        case Test::owned_by:
            if (!owner) owner = world_.owner(province);
            return *owner == r.name ? 1 : 0;
        case Test::core:
            if (!owner) owner = world_.owner(province);
            if (owner->empty()) return std::nullopt;
            return world_.has_core(province, *owner) == r.yes ? 1 : 0;
        case Test::occupied: {
            if (!owner) owner = world_.owner(province);
            if (owner->empty()) return std::nullopt;
            const std::string by = world_.controller(province);
            return (!by.empty() && by != *owner) == r.yes ? 1 : 0;
        }
        case Test::controlled_by:
            return world_.controller(province) == r.name ? 1 : 0;
        case Test::core_of:
            return world_.has_core(province, r.name) ? 1 : 0;
        case Test::religion:
        case Test::culture:
        case Test::trade_goods: {
            const std::string key = world_.province_key(province, r.test);
            if (key.empty()) return std::nullopt;
            return key == r.name ? 1 : 0;
        }
        case Test::subject:
        case Test::subject_of:
            return std::nullopt;  // the owner's: refused without owner = yes
    }
    return std::nullopt;
}

const Rule* Evaluator::match(std::int32_t province, std::int64_t* value) {
    std::optional<std::string> owner;
    for (std::size_t i = 0; i < mode_.rules.size(); ++i) {
        const Rule& r = mode_.rules[i];
        const std::optional<std::int64_t> v = read(i, province, owner);
        if (!v) continue;
        const bool passes = r.test == Test::always || (is_yes_no(r.test) ? *v != 0 : in_bounds(r, *v));
        if (!passes) continue;
        if (value != nullptr) *value = *v;
        return &r;
    }
    return nullptr;
}

std::vector<std::uint32_t> Evaluator::colors() {
    const std::int32_t limit = world_.province_limit();
    std::vector<std::uint32_t> out(limit > 0 ? static_cast<std::size_t>(limit) : 0, ANVIL_MAP_NO_COLOR);
    for (std::int32_t p = 1; p < limit; ++p) {
        std::int64_t value = 0;
        if (const Rule* r = match(p, &value)) out[static_cast<std::size_t>(p)] = color_at(*r, value);
    }
    return out;
}

namespace {

// `label` and then " <value>", the label cut at a character so the whole
// stays within a legend label's 64 bytes.
std::string with_value(const std::string& label, std::int64_t value) {
    const std::string tail = " " + decimal(value);
    std::size_t keep = std::min(label.size(), kLabelMax - tail.size());
    while (keep > 0 && keep < label.size() && (static_cast<unsigned char>(label[keep]) & 0xC0) == 0x80) --keep;
    return label.substr(0, keep) + tail;
}

}  // namespace

std::vector<LegendItem> legend_of(const FileMode& mode) {
    std::vector<LegendItem> out;
    const auto add = [&](std::uint32_t color, std::string label) {
        const bool seen = std::any_of(out.begin(), out.end(), [&](const LegendItem& i) {
            return i.color == color && i.label == label;
        });
        if (!seen && out.size() < ANVIL_MAP_LEGEND_MAX) out.push_back({color, std::move(label)});
    };
    for (const Rule& r : mode.rules) {
        if (r.scaled()) {
            add(r.color, with_value(r.label, *r.low));
            add(r.high_color, with_value(r.label, *r.high));
        } else {
            add(r.color, r.label);
        }
    }
    return out;
}

std::vector<std::string> enabled_mods(std::string_view json) {
    std::vector<std::string> out;
    const std::size_t list = json.find("\"enabled_mods\"");
    if (list == std::string_view::npos) return out;
    const std::size_t open = json.find('[', list);
    const std::size_t close = open == std::string_view::npos ? std::string_view::npos : json.find(']', open);
    if (close == std::string_view::npos) return out;
    const std::string_view items = json.substr(open + 1, close - open - 1);
    for (std::size_t q = items.find('"'); q != std::string_view::npos; q = items.find('"', q + 1)) {
        const std::size_t end = items.find('"', q + 1);
        if (end == std::string_view::npos) break;
        out.emplace_back(items.substr(q + 1, end - q - 1));
        q = end;
    }
    return out;
}

std::string mod_value(std::string_view text, std::string_view key) {
    for (std::size_t at = text.find(key); at != std::string_view::npos; at = text.find(key, at + 1)) {
        if (at != 0 && !std::isspace(static_cast<unsigned char>(text[at - 1]))) continue;
        std::size_t i = at + key.size();
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        if (i >= text.size() || text[i] != '=') continue;
        const std::size_t q = text.find('"', i);
        const std::size_t eol = text.find('\n', i);
        if (q == std::string_view::npos || q > eol) continue;
        const std::size_t end = text.find('"', q + 1);
        if (end == std::string_view::npos) return {};
        return std::string(text.substr(q + 1, end - q - 1));
    }
    return {};
}

std::vector<Found> playset_files(const std::filesystem::path& user, std::vector<std::string>& problems) {
    bool too_big = false;
    const std::string json = read_file(user / "dlc_load.json", too_big);
    // By lower-case file name: the mod that gives it last, and its place.
    std::map<std::string, std::pair<std::size_t, Found>> files;
    std::size_t index = 0;
    for (const std::string& entry : enabled_mods(json)) {
        ++index;
        const std::string mod = read_file(user / utf8_path(entry), too_big);
        std::string name = mod_value(mod, "name");
        if (name.empty() || !valid_utf8(name)) name = entry;
        const std::string path = mod_value(mod, "path");
        if (path.empty()) {
            if (!mod_value(mod, "archive").empty()) {
                problems.push_back(name + ": a zipped mod; its anvil/map_modes files are not read");
            }
            continue;
        }
        std::filesystem::path folder = utf8_path(path);
        if (folder.is_relative()) folder = user / folder;
        folder /= "anvil";
        folder /= "map_modes";
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec)) continue;
        for (const auto& item : std::filesystem::directory_iterator(folder, ec)) {
            if (!item.is_regular_file(ec)) continue;
            const std::filesystem::path& p = item.path();
            if (lower(path_utf8(p.extension())) != ".txt") continue;
            const std::string file = path_utf8(p.filename());
            files[lower(file)] = {index, Found{p, name + "/anvil/map_modes/" + file}};
        }
    }
    std::vector<std::pair<std::size_t, Found>> ordered;
    for (auto& [key, value] : files) ordered.push_back(std::move(value));
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Found> out;
    for (auto& [i, f] : ordered) out.push_back(std::move(f));
    return out;
}

std::vector<Stamp> stamps(const std::vector<Found>& found) {
    std::vector<Stamp> out;
    out.reserve(found.size());
    for (const Found& f : found) {
        Stamp s;
        s.path = f.path;
        std::error_code ec;
        s.size = std::filesystem::file_size(f.path, ec);
        if (ec) s.size = 0;
        s.time = std::filesystem::last_write_time(f.path, ec);
        if (ec) s.time = {};
        out.push_back(std::move(s));
    }
    return out;
}

Reload plan_reload(const std::vector<const FileMode*>& registered, const std::vector<FileMode>& fresh) {
    Reload out;
    std::set<std::string> known;
    for (const FileMode* m : registered) {
        known.insert(m->name);
        int index = -1;
        for (std::size_t i = 0; i < fresh.size(); ++i) {
            if (fresh[i].name == m->name) index = static_cast<int>(i);
        }
        out.fresh.push_back(index);
        if (index < 0) {
            out.missing.push_back(m->name);
            continue;
        }
        const FileMode& f = fresh[static_cast<std::size_t>(index)];
        if (f.label != m->label || f.description != m->description || f.tab != m->tab || f.sprite != m->sprite) {
            out.restart.push_back(m->name);
        }
    }
    for (const FileMode& f : fresh) {
        if (!known.contains(f.name)) out.added.push_back(f.name);
    }
    return out;
}

std::vector<FileMode> load(const std::vector<Found>& found, std::vector<std::string>& problems) {
    std::vector<FileMode> out;
    std::set<std::string> names;
    for (const Found& f : found) {
        bool too_big = false;
        const std::string text = read_file(f.path, too_big);
        if (too_big) {
            problems.push_back(f.source + ": larger than 1 MiB, not read");
            continue;
        }
        std::vector<FileMode> modes;
        parse(text, f.source, modes, problems);
        for (FileMode& m : modes) {
            if (!names.insert(m.name).second) {
                problems.push_back(m.source + ":" + std::to_string(m.line) + ": " + m.name +
                                   " is defined in an earlier file; this one is left out");
                continue;
            }
            out.push_back(std::move(m));
        }
    }
    return out;
}

}  // namespace anvil_map::files
