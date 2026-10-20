#include "crash_history.hpp"

#include <windows.h>

#include <algorithm>

namespace anvil {
namespace {

void put_utf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

bool hex4(std::string_view text, size_t at, uint32_t& out) {
    if (at + 4 > text.size()) return false;
    out = 0;
    for (size_t i = at; i < at + 4; ++i) {
        const char c = text[i];
        out <<= 4;
        if (c >= '0' && c <= '9') out |= c - '0';
        else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
        else return false;
    }
    return true;
}

// A JSON string literal at `at` (the opening quote); `at` moves past it.
std::optional<std::string> json_string(std::string_view text, size_t& at) {
    if (at >= text.size() || text[at] != '"') return std::nullopt;
    std::string out;
    for (size_t i = at + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            at = i + 1;
            return out;
        }
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= text.size()) break;
        switch (text[i]) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            uint32_t code = 0;
            if (!hex4(text, i + 1, code)) return std::nullopt;
            i += 4;
            uint32_t low = 0;
            if (code >= 0xD800 && code < 0xDC00 && text.substr(i + 1, 2) == "\\u" && hex4(text, i + 3, low) &&
                low >= 0xDC00 && low < 0xE000) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                i += 6;
            }
            put_utf8(out, code);
            break;
        }
        default: out += text[i]; break;  // \" \\ \/
        }
    }
    return std::nullopt;
}

// Where the value of a top-level key starts. The core writes every top-level
// key on its own line with two spaces of indentation.
size_t top_level_value(std::string_view json, std::string_view key) {
    const std::string pattern = "\n  \"" + std::string(key) + "\": ";
    const size_t at = json.find(pattern);
    return at == std::string_view::npos ? at : at + pattern.size();
}

std::string top_level_string(std::string_view json, std::string_view key) {
    size_t at = top_level_value(json, key);
    if (at == std::string_view::npos) return {};
    return json_string(json, at).value_or(std::string());
}

void skip_space(std::string_view text, size_t& at) {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t')) ++at;
}

}  // namespace

std::vector<std::string> crash_report_names(const std::filesystem::path& crash_dir) {
    std::vector<std::string> names;
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((crash_dir / L"*.json").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return names;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = found.cFileName;
        if (name.size() <= 5) continue;
        name.resize(name.size() - 5);
        std::string narrow;
        for (const wchar_t c : name) {
            if (c > 0x7F) {
                narrow.clear();
                break;  // not a name the core writes
            }
            narrow += static_cast<char>(c);
        }
        if (!narrow.empty()) names.push_back(std::move(narrow));
    } while (FindNextFileW(find, &found));
    FindClose(find);
    std::sort(names.begin(), names.end(), std::greater<>());
    return names;
}

CrashSummary parse_crash_summary(std::string_view json) {
    CrashSummary s;
    s.time = top_level_string(json, "time");
    s.level = top_level_string(json, "level");
    s.summary = top_level_string(json, "summary");
    if (size_t at = top_level_value(json, "before_game_ready"); at != std::string_view::npos) {
        s.before_game_ready = json.substr(at, 4) == "true";
    }
    if (size_t at = top_level_value(json, "plugins_involved"); at != std::string_view::npos && json[at] == '[') {
        ++at;
        for (;;) {
            skip_space(json, at);
            auto id = json_string(json, at);
            if (!id) break;
            s.plugins.push_back(std::move(*id));
            skip_space(json, at);
            if (at >= json.size() || json[at] != ',') break;
            ++at;
        }
    }
    // "issues": [{"plugin": "x", "url": "..."}, ...]: the first url.
    if (size_t at = top_level_value(json, "issues"); at != std::string_view::npos && json[at] == '[') {
        const size_t end = json.find(']', at);
        const size_t url = json.find("\"url\": ", at);
        if (url != std::string_view::npos && url < end) {
            size_t value = url + 7;
            s.issues = json_string(json, value).value_or(std::string());
        }
    }
    return s;
}

std::optional<CrashSummary> load_crash_summary(const std::filesystem::path& crash_dir, const std::string& name) {
    const auto json = read_text_file(crash_dir / (name + ".json"), 1 << 20);
    if (!json) return std::nullopt;
    CrashSummary s = parse_crash_summary(*json);
    s.name = name;
    return s;
}

bool crash_in_plugin(const CrashSummary& crash, std::string_view plugin_id) {
    if (crash.level != "in_plugin_code" && crash.level != "in_plugin_change") return false;
    return std::find(crash.plugins.begin(), crash.plugins.end(), plugin_id) != crash.plugins.end();
}

std::optional<std::string> read_text_file(const std::filesystem::path& file, size_t max_bytes) {
    const HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    std::optional<std::string> out;
    if (GetFileSizeEx(h, &size) && static_cast<unsigned long long>(size.QuadPart) <= max_bytes) {
        std::string text(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        if (text.empty() || ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) {
            text.resize(read);
            out = std::move(text);
        }
    }
    CloseHandle(h);
    return out;
}

std::string read_text_tail(const std::filesystem::path& file, size_t max_lines, size_t max_bytes) {
    const HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE || max_lines == 0 || max_bytes == 0) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        return {};
    }
    std::string text;
    LARGE_INTEGER size{};
    LARGE_INTEGER from{};
    if (GetFileSizeEx(h, &size)) {
        from.QuadPart = size.QuadPart > static_cast<LONGLONG>(max_bytes) ? size.QuadPart - max_bytes : 0;
        text.resize(static_cast<size_t>(size.QuadPart - from.QuadPart));
        DWORD read = 0;
        if (text.empty() || !SetFilePointerEx(h, from, nullptr, FILE_BEGIN) ||
            !ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) {
            read = 0;
        }
        text.resize(read);
    }
    CloseHandle(h);
    std::string_view tail = text;
    if (from.QuadPart == 0 && tail.substr(0, 3) == "\xEF\xBB\xBF") tail.remove_prefix(3);
    if (from.QuadPart > 0) {  // the first line is cut
        const size_t line = tail.find('\n');
        tail = line == std::string_view::npos ? std::string_view() : tail.substr(line + 1);
    }
    size_t lines = 0;
    for (size_t i = tail.size(); i > 0; --i) {
        if (tail[i - 1] == '\n' && i != tail.size() && ++lines == max_lines) return std::string(tail.substr(i));
    }
    return std::string(tail);
}

}  // namespace anvil
