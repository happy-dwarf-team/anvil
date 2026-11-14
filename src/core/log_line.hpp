// The shape of a line of anvil.log, header-only so that the Manager and the
// overlay (which does not link the core) read the levels Log::write writes.
#pragma once

#include <anvil/anvil_plugin.h>

#include <optional>
#include <string_view>

namespace anvil {

// A level's word in the line, five characters wide.
constexpr const char* log_level_word(AnvilLogLevel level) {
    switch (level) {
    case ANVIL_LOG_DEBUG: return "debug";
    case ANVIL_LOG_INFO: return "info ";
    case ANVIL_LOG_WARNING: return "WARN ";
    case ANVIL_LOG_ERROR: return "ERROR";
    }
    return "?    ";
}

// The level of a line as Log::write formats it: "[   12.3 ms]", the level's
// word, then "[source] message"; nothing for a line of another shape.
inline std::optional<AnvilLogLevel> log_line_level(std::string_view line) {
    const size_t at = line.find(" ms] ");
    if (line.empty() || line.front() != '[' || at == std::string_view::npos) return std::nullopt;
    const std::string_view word = line.substr(at + 5, 5);
    for (const AnvilLogLevel level : {ANVIL_LOG_DEBUG, ANVIL_LOG_INFO, ANVIL_LOG_WARNING, ANVIL_LOG_ERROR}) {
        if (word == log_level_word(level)) return level;
    }
    return std::nullopt;
}

}  // namespace anvil
