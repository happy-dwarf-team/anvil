#include "gfx_file.hpp"

#include <cctype>
#include <charconv>
#include <cstddef>

namespace model_reload {

namespace {

enum class Tok { open, close, equals, word, end };

struct Token {
    Tok kind = Tok::end;
    std::string_view text;  // a word without its quotes
    int line = 1;
};

// Clausewitz text: words (quoted or bare), `{`, `}`, `=`, and `#` comments to
// the end of the line outside quotes. `<`, `>` and the rest are bare words.
class Lexer {
public:
    explicit Lexer(std::string_view text) : s_(text) {
        if (s_.size() >= 3 && s_.substr(0, 3) == "\xEF\xBB\xBF") at_ = 3;  // a UTF-8 BOM
    }

    Token next() {
        for (;;) {
            while (at_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[at_]))) {
                if (s_[at_] == '\n') ++line_;
                ++at_;
            }
            if (at_ < s_.size() && s_[at_] == '#') {
                while (at_ < s_.size() && s_[at_] != '\n') ++at_;
                continue;
            }
            break;
        }
        Token t;
        t.line = line_;
        if (at_ >= s_.size()) return t;
        const char c = s_[at_];
        if (c == '{' || c == '}' || c == '=') {
            t.kind = c == '{' ? Tok::open : c == '}' ? Tok::close : Tok::equals;
            t.text = s_.substr(at_++, 1);
            return t;
        }
        t.kind = Tok::word;
        if (c == '"') {
            const std::size_t from = ++at_;
            while (at_ < s_.size() && s_[at_] != '"') {
                if (s_[at_] == '\n') ++line_;
                ++at_;
            }
            t.text = s_.substr(from, at_ - from);
            if (at_ < s_.size()) ++at_;
            return t;
        }
        const std::size_t from = at_;
        while (at_ < s_.size() && !std::isspace(static_cast<unsigned char>(s_[at_])) && s_[at_] != '{' &&
               s_[at_] != '}' && s_[at_] != '=' && s_[at_] != '#' && s_[at_] != '"') {
            ++at_;
        }
        t.text = s_.substr(from, at_ - from);
        return t;
    }

private:
    std::string_view s_;
    std::size_t at_ = 0;
    int line_ = 1;
};

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

bool parse_float(std::string_view text, float& out) {
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr != text.data();
}

// A tree of the file: enough for pdxmesh blocks at any depth.
class Parser {
public:
    explicit Parser(std::string_view text) : lex_(text) { advance(); }

    GfxFile run() {
        GfxFile out;
        block(out, 0, false);
        if (tok_.kind != Tok::end || bad_) out.unbalanced = true;
        if (out.unbalanced) out.meshes.clear();
        return out;
    }

private:
    void advance() { tok_ = lex_.next(); }

    // Skips one value: a word or a whole block.
    void skip_value(int depth) {
        if (tok_.kind == Tok::open) {
            advance();
            GfxFile ignored;
            block(ignored, depth + 1, true);
            return;
        }
        if (tok_.kind == Tok::word) advance();
    }

    // The entries of a block up to its `}` (inner) or the end of the file.
    void block(GfxFile& out, int depth, bool inner) {
        if (depth > 64) {
            bad_ = true;
            return;
        }
        for (;;) {
            if (bad_) return;
            if (tok_.kind == Tok::end) {
                if (inner) bad_ = true;
                return;
            }
            if (tok_.kind == Tok::close) {
                if (!inner) {
                    bad_ = true;
                    return;
                }
                advance();
                return;
            }
            if (tok_.kind == Tok::open) {  // a bare block in a list
                advance();
                block(out, depth + 1, true);
                continue;
            }
            if (tok_.kind == Tok::equals) {  // a stray '='
                advance();
                continue;
            }
            const Token key = tok_;
            advance();
            if (tok_.kind != Tok::equals) continue;  // a bare word in a list
            advance();
            if (tok_.kind == Tok::open && iequals(key.text, "pdxmesh")) {
                advance();
                MeshEntry e;
                e.line = key.line;
                mesh(e, depth + 1);
                out.meshes.push_back(std::move(e));
            } else if (tok_.kind == Tok::open) {
                advance();
                block(out, depth + 1, true);
            } else {
                skip_value(depth);
            }
        }
    }

    void note_other(MeshEntry& e, std::string_view key) {
        std::string k(key);
        for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const std::string& o : e.other_keys) {
            if (o == k) return;
        }
        e.other_keys.push_back(std::move(k));
    }

    // The fields of one pdxmesh block; the last of a field wins, as in the game.
    void mesh(MeshEntry& e, int depth) {
        for (;;) {
            if (bad_) return;
            if (tok_.kind == Tok::end) {
                bad_ = true;
                return;
            }
            if (tok_.kind == Tok::close) {
                advance();
                return;
            }
            if (tok_.kind != Tok::word) {
                e.malformed = true;
                skip_value(depth);
                if (tok_.kind == Tok::equals) advance();
                continue;
            }
            const Token key = tok_;
            advance();
            if (tok_.kind != Tok::equals) {
                e.malformed = true;
                continue;
            }
            advance();
            const bool is_block = tok_.kind == Tok::open;
            if (iequals(key.text, "animation") && is_block) {
                advance();
                AnimationEntry a;
                animation(a, e, depth + 1);
                e.animations.push_back(std::move(a));
                continue;
            }
            if (is_block) {
                note_other(e, key.text);
                skip_value(depth);
                continue;
            }
            if (tok_.kind != Tok::word) {
                e.malformed = true;
                continue;
            }
            const std::string_view v = tok_.text;
            advance();
            if (iequals(key.text, "name")) {
                e.name = std::string(v);
            } else if (iequals(key.text, "file")) {
                e.file = std::string(v);
                for (char& c : e.file) {
                    if (c == '\\') c = '/';
                }
                e.has_file = true;
            } else if (iequals(key.text, "scale")) {
                if (!parse_float(v, e.scale)) e.malformed = true;
            } else if (iequals(key.text, "cull_distance")) {
                if (!parse_float(v, e.cull_distance)) e.malformed = true;
            } else {
                note_other(e, key.text);
            }
        }
    }

    void animation(AnimationEntry& a, MeshEntry& e, int depth) {
        for (;;) {
            if (bad_) return;
            if (tok_.kind == Tok::end) {
                bad_ = true;
                return;
            }
            if (tok_.kind == Tok::close) {
                advance();
                return;
            }
            if (tok_.kind != Tok::word) {
                e.malformed = true;
                skip_value(depth);
                if (tok_.kind == Tok::equals) advance();
                continue;
            }
            const Token key = tok_;
            advance();
            if (tok_.kind != Tok::equals) {
                e.malformed = true;
                continue;
            }
            advance();
            if (tok_.kind != Tok::word) {
                e.malformed = true;
                skip_value(depth);
                continue;
            }
            if (iequals(key.text, "id")) {
                a.id = std::string(tok_.text);
            } else if (iequals(key.text, "type")) {
                a.type = std::string(tok_.text);
            } else {
                e.malformed = true;
            }
            advance();
        }
    }

    Lexer lex_;
    Token tok_;
    bool bad_ = false;
};

}  // namespace

GfxFile parse_gfx(std::string_view text) { return Parser(text).run(); }

std::uint64_t animation_hash(std::string_view id) {
    std::uint32_t h = 0x811C9DC5u;
    for (const char c : id) {
        h = (h * 0x01000193u) ^ static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<signed char>(c)));
    }
    return h;
}

}  // namespace model_reload
