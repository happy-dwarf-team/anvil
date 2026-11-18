#include "log.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace anvil {
namespace {

struct LogState {
    std::mutex mutex;
    std::vector<std::string> pending;  // lines written before open()
    std::vector<std::string> warnings;  // "[source] message", at most Log::kMaxWarnings
    HANDLE file = INVALID_HANDLE_VALUE;
    // open() could not open the file: the game's own restart starts the new
    // process before the old one has closed its log. Writes try again until then.
    std::filesystem::path path;
    ULONGLONG retry_until = 0;
    ULONGLONG next_try = 0;
    ULONGLONG first_try = 0;
    AnvilLogLevel minimum = ANVIL_LOG_INFO;
    LARGE_INTEGER start{};
    LARGE_INTEGER frequency{};

    LogState() {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }
};

LogState& state() {
    static LogState s;
    return s;
}

const char* level_name(AnvilLogLevel level) { return log_level_word(level); }

void write_raw(HANDLE file, const std::string& line) {
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

constexpr ULONGLONG kRetryMs = 60'000;      // how long a log in use is waited for
constexpr ULONGLONG kRetryEveryMs = 250;

// Under the log's lock: keeps the last log as anvil.prev.log, opens a new one
// and writes what waited for it.
bool try_open(LogState& s) {
    std::filesystem::path previous = s.path;
    previous.replace_extension(L".prev.log");
    MoveFileExW(s.path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
    s.file = CreateFileW(s.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
    if (s.file == INVALID_HANDLE_VALUE) {
        return false;
    }
    static const char bom[] = "\xEF\xBB\xBF";
    write_raw(s.file, bom);
    for (const auto& line : s.pending) {
        write_raw(s.file, line);
    }
    s.pending.clear();
    s.pending.shrink_to_fit();
    return true;
}

}  // namespace

Log& Log::instance() {
    static Log log;
    return log;
}

void Log::open(const std::filesystem::path& file) {
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    if (s.file != INVALID_HANDLE_VALUE) {
        return;
    }
    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    s.path = file;
    if (try_open(s) || GetLastError() != ERROR_SHARING_VIOLATION) {
        return;
    }
    // Another process still writes it: the one the game's own restart replaces.
    s.first_try = GetTickCount64();
    s.retry_until = s.first_try + kRetryMs;
    s.next_try = s.first_try + kRetryEveryMs;
}

namespace {

// The length of the UTF-8 sequence at `at`, 0 when it is not one (overlong
// forms, surrogates and values past Unicode included).
size_t utf8_length(std::string_view text, size_t at) {
    const auto c = static_cast<unsigned char>(text[at]);
    if (c < 0x80) return 1;
    size_t length = 0;
    uint32_t code = 0;
    uint32_t least = 0;
    if ((c & 0xE0) == 0xC0) {
        length = 2, code = c & 0x1F, least = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        length = 3, code = c & 0x0F, least = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        length = 4, code = c & 0x07, least = 0x10000;
    } else {
        return 0;
    }
    if (at + length > text.size()) return 0;
    for (size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[at + i]);
        if ((next & 0xC0) != 0x80) return 0;
        code = (code << 6) | (next & 0x3F);
    }
    if (code < least || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return 0;
    return length;
}

void append_utf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

}  // namespace

std::string log_text(std::string_view text) {
    size_t valid = 0;
    while (valid < text.size()) {
        const size_t length = utf8_length(text, valid);
        if (length == 0) break;
        valid += length;
    }
    if (valid == text.size()) return std::string(text);
    // 0x80-0x9F of Windows-1252; its five unassigned bytes stay C1 controls.
    static constexpr uint16_t kHigh[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                           0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                                           0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                           0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
    std::string out(text.substr(0, valid));
    out.reserve(text.size() + text.size() / 2);
    for (size_t i = valid; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        append_utf8(out, c < 0x80 ? c : c < 0xA0 ? kHigh[c - 0x80] : c);
    }
    return out;
}

namespace {

std::string format_line(AnvilLogLevel level, std::string_view source, std::string_view message) {
    LogState& s = state();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double ms = static_cast<double>(now.QuadPart - s.start.QuadPart) * 1000.0 /
                      static_cast<double>(s.frequency.QuadPart ? s.frequency.QuadPart : 1);
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "[%9.1f ms] %s ", ms, level_name(level));
    std::string line = prefix;
    line += '[';
    line += log_text(source);
    line += "] ";
    line += log_text(message);
    line += "\r\n";
    return line;
}

// Under the log's lock.
void keep_warning(LogState& s, AnvilLogLevel level, std::string_view source, std::string_view message) {
    if (level < ANVIL_LOG_WARNING || s.warnings.size() >= Log::kMaxWarnings) return;
    std::string line = "[";
    line += log_text(source);
    line += "] ";
    line += log_text(message);
    s.warnings.push_back(std::move(line));
}

// Under the log's lock.
void append_line(LogState& s, std::string line) {
    if (s.file == INVALID_HANDLE_VALUE && s.retry_until != 0) {
        const ULONGLONG now = GetTickCount64();
        if (now >= s.next_try) {
            s.next_try = now + kRetryEveryMs;
            if (try_open(s)) {
                write_raw(s.file, format_line(ANVIL_LOG_INFO, "anvil",
                                              "anvil.log was still in use by the process this one replaced "
                                              "(the game's own restart); opened " +
                                                  std::to_string(now - s.first_try) + " ms later"));
                s.retry_until = 0;
            } else if (now >= s.retry_until) {
                s.retry_until = 0;
            }
        }
    }
    if (s.file == INVALID_HANDLE_VALUE) {
        if (s.pending.size() < 4096) {
            s.pending.push_back(std::move(line));
        }
        return;
    }
    write_raw(s.file, line);
}

}  // namespace

void Log::write(AnvilLogLevel level, std::string_view source, std::string_view message) {
    LogState& s = state();
    if (level < s.minimum) {
        if (level >= ANVIL_LOG_WARNING) {
            std::lock_guard lock(s.mutex);
            keep_warning(s, level, source, message);
        }
        return;
    }
    std::string line = format_line(level, source, message);
    std::lock_guard lock(s.mutex);
    keep_warning(s, level, source, message);
    append_line(s, std::move(line));
}

bool Log::try_write(AnvilLogLevel level, std::string_view source, std::string_view message) {
    LogState& s = state();
    if (level < s.minimum) {
        return true;
    }
    std::string line = format_line(level, source, message);
    std::unique_lock lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }
    keep_warning(s, level, source, message);
    append_line(s, std::move(line));
    return true;
}

void Log::set_min_level(AnvilLogLevel level) {
    state().minimum = level;
}

bool Log::is_open() const {
    return state().file != INVALID_HANDLE_VALUE;
}

std::vector<std::string> Log::warnings() const {
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    return s.warnings;
}

size_t Log::warning_count() const {
    LogState& s = state();
    std::lock_guard lock(s.mutex);
    return s.warnings.size();
}

void log_info(std::string_view message) { Log::instance().write(ANVIL_LOG_INFO, "anvil", message); }
void log_warning(std::string_view message) { Log::instance().write(ANVIL_LOG_WARNING, "anvil", message); }
void log_error(std::string_view message) { Log::instance().write(ANVIL_LOG_ERROR, "anvil", message); }
void log_debug(std::string_view message) { Log::instance().write(ANVIL_LOG_DEBUG, "anvil", message); }

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                          nullptr);
    std::string out(static_cast<std::size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes, nullptr,
                            nullptr);
    }
    return out;
}

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(chars > 0 ? chars : 0), L'\0');
    if (chars > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), chars);
    }
    return out;
}

}  // namespace anvil
