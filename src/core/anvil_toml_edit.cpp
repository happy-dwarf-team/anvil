#include "anvil_toml_edit.hpp"

#include "log.hpp"

#include <toml++/toml.hpp>
#include <windows.h>

#include <algorithm>

namespace anvil {
namespace {

constexpr std::string_view kKey = "disabled_plugins";
constexpr std::string_view kBom = "\xEF\xBB\xBF";

bool valid_id(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

// Byte offset of the start of 1-based `line`; npos past the end.
size_t line_start(std::string_view text, uint32_t line) {
    size_t at = 0;
    for (uint32_t i = 1; i < line; ++i) {
        at = text.find('\n', at);
        if (at == std::string_view::npos) return at;
        ++at;
    }
    return at;
}

// Past the end of a string that starts at `at` (a quote), or npos.
size_t skip_string(std::string_view text, size_t at) {
    const char quote = text[at];
    const bool basic = quote == '"';
    if (text.substr(at, 3) == std::string(3, quote)) {
        for (size_t i = at + 3; i < text.size(); ++i) {
            if (basic && text[i] == '\\') {
                ++i;
                continue;
            }
            if (text.substr(i, 3) == std::string(3, quote)) {
                // A closing delimiter may carry up to two more quotes.
                size_t end = i + 3;
                while (end < text.size() && end < i + 5 && text[end] == quote) ++end;
                return end;
            }
        }
        return std::string_view::npos;
    }
    for (size_t i = at + 1; i < text.size(); ++i) {
        if (basic && text[i] == '\\') {
            ++i;
            continue;
        }
        if (text[i] == quote) return i + 1;
        if (text[i] == '\n') return std::string_view::npos;
    }
    return std::string_view::npos;
}

// Where the id goes in the list that opens at `open`, and what to insert.
struct Insertion {
    size_t at = std::string_view::npos;
    std::string text;
};

Insertion insertion_in_list(std::string_view text, size_t open, std::string_view id) {
    size_t after_last = open + 1;
    char last = '[';  // '[': empty so far, ',': after a comma, 'v': after a value
    int depth = 1;
    for (size_t i = open + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '#') {
            const size_t end = text.find('\n', i);
            if (end == std::string_view::npos) break;
            i = end;
        } else if (c == '"' || c == '\'') {
            const size_t end = skip_string(text, i);
            if (end == std::string_view::npos) break;
            if (depth == 1) last = 'v', after_last = end;
            i = end - 1;
        } else if (c == '[' || c == '{') {
            ++depth;
        } else if (c == ']' || c == '}') {
            if (--depth == 0) {
                Insertion out;
                out.at = after_last;
                const std::string quoted = "\"" + std::string(id) + "\"";
                out.text = last == '[' ? quoted : last == ',' ? " " + quoted : ", " + quoted;
                return out;
            }
            if (depth == 1) last = 'v', after_last = i + 1;
        } else if (c == ',') {
            if (depth == 1) last = ',', after_last = i + 1;
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            if (depth == 1) last = 'v', after_last = i + 1;
        }
    }
    return {};
}

// The line of the first table header ([table] or [[array]]), 0 when none.
uint32_t first_table_line(const toml::table& table) {
    uint32_t first = 0;
    for (const auto& [key, node] : table) {
        uint32_t line = 0;
        if (const auto* t = node.as_table(); t != nullptr && !t->is_inline()) {
            line = node.source().begin.line;
            const uint32_t nested = first_table_line(*t);
            if (nested != 0 && (line == 0 || nested < line)) line = nested;
        } else if (const auto* a = node.as_array(); a != nullptr && a->is_array_of_tables()) {
            line = node.source().begin.line;
        }
        if (line != 0 && (first == 0 || line < first)) first = line;
    }
    return first;
}

}  // namespace

std::optional<std::string> with_disabled_plugin(std::string_view input, std::string_view id, std::string& error) {
    if (!valid_id(id)) {
        error = "'" + std::string(id) + "' is not a plugin id";
        return std::nullopt;
    }
    const bool bom = input.substr(0, kBom.size()) == kBom;
    const std::string_view text = bom ? input.substr(kBom.size()) : input;
    toml::table before;
    try {
        before = toml::parse(text);
    } catch (const toml::parse_error& e) {
        error = "anvil.toml does not parse (line " + std::to_string(e.source().begin.line) +
                "): " + std::string(e.description());
        return std::nullopt;
    }
    toml::array expected;
    std::string edited;
    if (const toml::node* node = before.get(kKey)) {
        const toml::array* list = node->as_array();
        if (list == nullptr) {
            error = "disabled_plugins in anvil.toml is not a list";
            return std::nullopt;
        }
        for (const auto& item : *list) {
            if (item.value<std::string>() == std::string(id)) return std::string(input);  // already listed
        }
        expected = *list;
        // The value starts on the key's line, after its '='.
        const size_t line = line_start(text, node->source().begin.line);
        const size_t equals = line == std::string_view::npos ? line : text.find('=', line);
        const size_t open = equals == std::string_view::npos ? equals : text.find('[', equals);
        const Insertion insertion =
            open == std::string_view::npos ? Insertion{} : insertion_in_list(text, open, id);
        if (insertion.at == std::string_view::npos) {
            error = "cannot find the list of disabled_plugins in anvil.toml";
            return std::nullopt;
        }
        edited = std::string(text.substr(0, insertion.at)) + insertion.text + std::string(text.substr(insertion.at));
    } else {
        const std::string newline = text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
        const std::string entry = std::string(kKey) + " = [\"" + std::string(id) + "\"]" + newline;
        const uint32_t table = first_table_line(before);
        const size_t at = table == 0 ? std::string_view::npos : line_start(text, table);
        if (at == std::string_view::npos) {
            edited = std::string(text);
            if (!edited.empty() && edited.back() != '\n') edited += newline;
            edited += entry;
        } else {
            edited = std::string(text.substr(0, at)) + entry + newline + std::string(text.substr(at));
        }
    }
    expected.push_back(std::string(id));
    // The edit must read back as the same file plus the id.
    try {
        const toml::table after = toml::parse(edited);
        toml::table want = before;
        want.insert_or_assign(kKey, expected);
        if (!(after == want)) {
            error = "the edited anvil.toml would not read back as expected; not written";
            return std::nullopt;
        }
    } catch (const toml::parse_error& e) {
        error = "the edited anvil.toml would not parse: " + std::string(e.description()) + "; not written";
        return std::nullopt;
    }
    return bom ? std::string(kBom) + edited : edited;
}

bool add_disabled_plugin(const std::filesystem::path& file, std::string_view id, std::string& error) {
    std::string text;
    const HANDLE in = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(in, &size) && size.QuadPart < (16 << 20);
        if (ok) {
            text.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            ok = text.empty() || (ReadFile(in, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) &&
                                  read == text.size());
        }
        CloseHandle(in);
        if (!ok) {
            error = "cannot read " + narrow(file.wstring());
            return false;
        }
    } else if (const DWORD code = GetLastError(); code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
        error = "cannot open " + narrow(file.wstring()) + " (error " + std::to_string(code) + ")";
        return false;
    }
    const std::optional<std::string> edited = with_disabled_plugin(text, id, error);
    if (!edited) return false;
    if (*edited == text && in != INVALID_HANDLE_VALUE) return true;  // already listed

    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    std::filesystem::path temporary = file;
    temporary += L".tmp";
    const HANDLE out =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        error = "cannot write " + narrow(temporary.wstring()) + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD written = 0;
    const bool wrote = WriteFile(out, edited->data(), static_cast<DWORD>(edited->size()), &written, nullptr) &&
                       written == edited->size() && FlushFileBuffers(out);
    CloseHandle(out);
    if (!wrote ||
        !MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "cannot replace " + narrow(file.wstring()) + " (error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

}  // namespace anvil
