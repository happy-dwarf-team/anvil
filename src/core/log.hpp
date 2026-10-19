#pragma once

#include <anvil/anvil_plugin.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace anvil {

// Process-wide log. Lines are buffered in memory until open() is called with
// the log path (the previous log is rotated to *.prev.log); afterwards every
// line is appended and flushed. Thread-safe.
class Log {
public:
    static Log& instance();

    void open(const std::filesystem::path& file);
    void write(AnvilLogLevel level, std::string_view source, std::string_view message);
    // The same without waiting: false when another thread holds the log (at
    // process exit that thread may have been killed holding it).
    bool try_write(AnvilLogLevel level, std::string_view source, std::string_view message);
    void set_min_level(AnvilLogLevel level);
    bool is_open() const;

private:
    Log() = default;
};

void log_info(std::string_view message);
void log_warning(std::string_view message);
void log_error(std::string_view message);
void log_debug(std::string_view message);

std::string narrow(const std::wstring& text);
std::wstring widen(std::string_view text);

}  // namespace anvil
