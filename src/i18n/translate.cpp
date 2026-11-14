#include "i18n/translate.hpp"

#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

namespace anvil::i18n {

extern const Translation kFrench[];
extern const size_t kFrenchSize;
extern const Translation kGerman[];
extern const size_t kGermanSize;
extern const Translation kSpanish[];
extern const size_t kSpanishSize;
extern const Translation kSimplifiedChinese[];
extern const size_t kSimplifiedChineseSize;
extern const Translation kTraditionalChinese[];
extern const size_t kTraditionalChineseSize;
extern const Translation kJapanese[];
extern const size_t kJapaneseSize;
extern const Translation kKorean[];
extern const size_t kKoreanSize;
extern const Translation kPolish[];
extern const size_t kPolishSize;

namespace {

std::atomic<Language> g_language{Language::English};

using Index = std::unordered_map<std::string_view, const char*>;

Index build(Language language) {
    Index index;
    const Catalog c = catalog(language);
    index.reserve(c.size);
    for (size_t i = 0; i < c.size; ++i) index.emplace(c.entries[i].english, c.entries[i].translated);
    return index;
}

// Built on the first lookup of a language: only the player's is ever built.
const Index& index_of(Language language) {
    static const Index none;
    switch (language) {
    case Language::French: { static const Index french = build(Language::French); return french; }
    case Language::German: { static const Index german = build(Language::German); return german; }
    case Language::Spanish: { static const Index spanish = build(Language::Spanish); return spanish; }
    case Language::SimplifiedChinese: { static const Index hans = build(Language::SimplifiedChinese); return hans; }
    case Language::TraditionalChinese: { static const Index hant = build(Language::TraditionalChinese); return hant; }
    case Language::Japanese: { static const Index japanese = build(Language::Japanese); return japanese; }
    case Language::Korean: { static const Index korean = build(Language::Korean); return korean; }
    case Language::Polish: { static const Index polish = build(Language::Polish); return polish; }
    case Language::English: break;
    }
    return none;
}

}  // namespace

Catalog catalog(Language language) {
    switch (language) {
    case Language::French: return {kFrench, kFrenchSize};
    case Language::German: return {kGerman, kGermanSize};
    case Language::Spanish: return {kSpanish, kSpanishSize};
    case Language::SimplifiedChinese: return {kSimplifiedChinese, kSimplifiedChineseSize};
    case Language::TraditionalChinese: return {kTraditionalChinese, kTraditionalChineseSize};
    case Language::Japanese: return {kJapanese, kJapaneseSize};
    case Language::Korean: return {kKorean, kKoreanSize};
    case Language::Polish: return {kPolish, kPolishSize};
    case Language::English: break;
    }
    return {nullptr, 0};
}

void set_language(Language language) { g_language.store(language, std::memory_order_relaxed); }

Language language() { return g_language.load(std::memory_order_relaxed); }

const char* tr(Language language, const char* english) {
    if (english == nullptr || language == Language::English) return english;
    const Index& index = index_of(language);
    const auto it = index.find(english);
    return it != index.end() && it->second[0] != '\0' ? it->second : english;
}

const char* tr(const char* english) { return tr(language(), english); }

std::string trf(const char* english, std::initializer_list<std::string_view> args) {
    return i18n::format(tr(english), std::vector<std::string>(args.begin(), args.end()));
}

int plural_forms(Language language) {
    switch (language) {
    case Language::SimplifiedChinese:
    case Language::TraditionalChinese:
    case Language::Japanese:
    case Language::Korean: return 1;
    case Language::Polish: return 3;
    case Language::English:
    case Language::French:
    case Language::German:
    case Language::Spanish: break;
    }
    return 2;
}

// The CLDR rules of each language's cardinal numbers, for whole numbers.
int plural_form(Language language, long long n) {
    const long long a = n < 0 ? -n : n;
    switch (language) {
    case Language::SimplifiedChinese:
    case Language::TraditionalChinese:
    case Language::Japanese:
    case Language::Korean: return 0;
    case Language::French: return a <= 1 ? 0 : 1;
    case Language::Polish:
        if (a == 1) return 0;
        if (a % 10 >= 2 && a % 10 <= 4 && (a % 100 < 12 || a % 100 > 14)) return 1;
        return 2;
    case Language::English:
    case Language::German:
    case Language::Spanish: break;
    }
    return a == 1 ? 0 : 1;
}

std::string trn(Language language, const char* forms, long long n, std::initializer_list<std::string_view> args) {
    // A translation without enough forms falls back to its last one.
    const std::string_view all = tr(language, forms);
    const bool english = all.data() == std::string_view(forms).data();
    int form = english ? plural_form(Language::English, n) : plural_form(language, n);
    size_t begin = 0;
    for (; form > 0; --form) {
        const size_t bar = all.find('|', begin);
        if (bar == std::string_view::npos) break;
        begin = bar + 1;
    }
    const size_t end = all.find('|', begin);
    const std::string_view chosen = all.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
    std::vector<std::string> values{std::to_string(n)};
    values.insert(values.end(), args.begin(), args.end());
    return i18n::format(std::string(chosen), values);
}

std::string trn(const char* forms, long long n, std::initializer_list<std::string_view> args) {
    return trn(language(), forms, n, args);
}

std::string text(Language language, const Message& message) {
    if (message.code.empty()) return message.text;
    std::vector<std::string> args;
    for (const Message& a : message.args) args.push_back(text(language, a));
    const char* pattern = pattern_of(message.code);
    if (pattern == nullptr) return english(message);
    return i18n::format(tr(language, pattern), args);
}

std::string text(const Message& message) { return text(language(), message); }

}  // namespace anvil::i18n
