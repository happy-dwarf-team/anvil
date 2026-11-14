// Where Chinese, Japanese and Korean lines may break (src/ui/text_wrap.hpp
// draws with it). ImGui wraps only at spaces and a few punctuation marks, so a
// Japanese sentence wraps at its 、 and 。 and leaves the line short. Here a
// line may also end between two characters when either is an ideograph or
// kana, except before a mark that may not start a line (、。」ー and the small
// kana) and after one that may not end it (「（): the rules of kinsoku shori.
// Korean keeps its spaces as word breaks. A path breaks after \\ or /. No ImGui: tests/ui_model_tests.cpp
// checks it.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace anvil::ui {

// The code point at s[i] and its length in bytes (1 for a broken sequence).
inline uint32_t utf8_at(std::string_view s, size_t i, size_t& length) {
    const auto c = static_cast<unsigned char>(s[i]);
    length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (length == 1 || i + length > s.size()) {
        length = 1;
        return c;
    }
    uint32_t cp = length == 2 ? c & 0x1F : length == 3 ? c & 0x0F : c & 0x07;
    for (size_t k = 1; k < length; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    return cp;
}

// Ideographs, kana, CJK punctuation and full-width forms: a line may break
// before or after them. Hangul is not among them: Korean breaks at spaces.
inline bool breaks_anywhere(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
           (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x3FFFF);
}

// Kinsoku: what may not start a line, and what may not end one.
inline bool no_line_start(uint32_t c) {
    switch (c) {
    case U'、': case U'。': case U'，': case U'．': case U'・': case U'：': case U'；': case U'？': case U'！':
    case U'ー': case U'」': case U'』': case U'）': case U'】': case U'〉': case U'》': case U'〕': case U'］': case U'｝':
    case U'〟': case U'々': case U'ゝ': case U'ゞ': case U'ヽ': case U'ヾ': case U'…':
    case U'ぁ': case U'ぃ': case U'ぅ': case U'ぇ': case U'ぉ': case U'っ': case U'ゃ': case U'ゅ': case U'ょ': case U'ゎ':
    case U'ァ': case U'ィ': case U'ゥ': case U'ェ': case U'ォ': case U'ッ': case U'ャ': case U'ュ': case U'ョ': case U'ヮ':
    case U'ヵ': case U'ヶ':
    case U',': case U'.': case U':': case U';': case U'!': case U'?': case U')': case U']': case U'}': case U'%':
        return true;
    default:
        return false;
    }
}

inline bool no_line_end(uint32_t c) {
    switch (c) {
    case U'「': case U'『': case U'（': case U'【': case U'〈': case U'《': case U'〔': case U'［': case U'｛': case U'〝':
    case U'(': case U'[': case U'{':
        return true;
    default:
        return false;
    }
}

// Text that needs these breaks: CJK text, or a Windows path (a path breaks
// after its separators, not inside a folder's name).
inline bool needs_own_breaks(std::string_view text) {
    for (size_t i = 0, n = 0; i < text.size(); i += n) {
        const uint32_t c = utf8_at(text, i, n);
        if (c == '\\' || breaks_anywhere(c)) return true;
    }
    return false;
}

inline bool has_cjk_breaks(std::string_view text) {
    for (size_t i = 0, n = 0; i < text.size(); i += n) {
        if (breaks_anywhere(utf8_at(text, i, n))) return true;
    }
    return false;
}

// Where each line of `text` begins when it wraps at `width`; `advance(begin,
// end)` measures one character. A line that cannot break anywhere breaks
// before the character that overflows, as ImGui does.
template <class Advance>
std::vector<size_t> wrap_starts(std::string_view text, float width, Advance advance) {
    std::vector<size_t> starts{0};
    float line = 0;                    // the width of the line so far
    size_t brk = std::string_view::npos;  // where the line may end...
    size_t brk_next = 0;               // ...and where the next one then begins
    float after_brk = 0;               // the width from brk_next to here
    uint32_t prev = 0;
    for (size_t i = 0, n = 0; i < text.size(); i += n) {
        const uint32_t c = utf8_at(text, i, n);
        if (c == '\n') {
            starts.push_back(i + n);
            line = after_brk = 0;
            brk = std::string_view::npos;
            prev = 0;
            continue;
        }
        const bool line_start = i == starts.back();
        if (!line_start) {
            if (c == ' ') {
                // A space ends the line before it; the next line starts after the spaces.
                size_t next = i;
                while (next < text.size() && text[next] == ' ') ++next;
                brk = i;
                brk_next = next;
                after_brk = 0;
            } else if ((prev == '\\' || prev == '/') && c != '\\' && c != '/') {
                // A path breaks after a separator rather than inside a folder's name.
                brk = i;
                brk_next = i;
                after_brk = 0;
            } else if (prev != ' ' && (breaks_anywhere(c) || breaks_anywhere(prev)) && !no_line_start(c) && !no_line_end(prev)) {
                brk = i;
                brk_next = i;
                after_brk = 0;
            }
        }
        const float w = advance(text.data() + i, text.data() + i + n);
        if (!line_start && c != ' ' && line + w > width) {
            if (brk != std::string_view::npos && brk > starts.back()) {
                starts.push_back(brk_next);
                line = after_brk;
            } else {
                starts.push_back(i);
                line = 0;
            }
            brk = std::string_view::npos;
            after_brk = 0;
        }
        line += w;
        if (i >= brk_next) after_brk += w;
        prev = c;
    }
    return starts;
}

}  // namespace anvil::ui
