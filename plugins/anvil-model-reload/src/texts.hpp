// The model reload window's words in the player's language. anvil-overlay.panel
// translates nothing, so the panel carries its own table, as the inspector and
// the script profiler do. anvil_model_reload_tests checks that every text the
// panel uses has every language, with the same placeholders. Console commands
// and file names stay as the game writes them.
#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

namespace model_reload {

// The languages of anvil-overlay.panel's language(), in the order of a
// TextEntry's translations.
enum class Lang { en, fr, de, es, zh_hans, zh_hant, ja, ko, pl };
inline constexpr std::size_t kTranslations = 8;  // every language but English

Lang lang_of(std::string_view code);  // "en", "fr", ..., "zh-Hans"; English when unknown

struct TextEntry {
    const char* english;
    const char* translated[kTranslations];  // fr, de, es, zh-Hans, zh-Hant, ja, ko, pl
};
const TextEntry* text_entries(std::size_t& count);

// Marks an English text that is translated where it is used.
constexpr const char* N_(const char* english) { return english; }

struct Texts {
    Lang lang = Lang::en;

    // The text in `lang`; the English words when the table has none.
    const char* operator()(const char* english) const;
    // Then {0}, {1}... replaced by `args`.
    std::string f(const char* english, std::initializer_list<std::string_view> args) const;
};

}  // namespace model_reload
