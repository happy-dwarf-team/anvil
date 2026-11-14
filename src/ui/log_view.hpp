// anvil.log as the Manager's Log page and the overlay's Log tab draw it
// (docs/ui-design-system.md, "Type"): the lines in the monospace font, a
// warning's and an error's glyph in a gutter, and chips that filter to them.
#pragma once

#include "core/log_line.hpp"
#include "i18n/translate.hpp"
#include "ui/forge_widgets.hpp"
#include "ui/plugin_widgets.hpp"

#include <imgui.h>

#include <string>
#include <string_view>
#include <vector>

namespace anvil::ui {

// A line of the log and its level (an AnvilLogLevel, -1 for a line of another shape).
struct LogLine {
    std::string_view text;
    int level = -1;
};

// `text` in lines, each pointing into it: build again whenever `text` changes.
inline std::vector<LogLine> split_log(std::string_view text) {
    std::vector<LogLine> lines;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto level = log_line_level(line);
        lines.push_back({line, level ? static_cast<int>(*level) : -1});
    }
    return lines;
}

// Which levels the lines are filtered to; none on shows every line.
struct LogFilter {
    bool warnings = false;
    bool errors = false;
    bool shows(int level) const {
        return (!warnings && !errors) || (warnings && level == ANVIL_LOG_WARNING) || (errors && level == ANVIL_LOG_ERROR);
    }
};

// The chips "Warnings (n)" and "Errors (n)", while the log has such lines or one is on.
inline void log_chips(const std::vector<LogLine>& lines, LogFilter& filter) {
    size_t warnings = 0, errors = 0;
    for (const LogLine& l : lines) {
        warnings += l.level == ANVIL_LOG_WARNING ? 1 : 0;
        errors += l.level == ANVIL_LOG_ERROR ? 1 : 0;
    }
    if (warnings + errors == 0 && !filter.warnings && !filter.errors) return;
    if (chip((i18n::trf("Warnings ({0})", {std::to_string(warnings)}) + "##warnings").c_str(), filter.warnings)) {
        filter.warnings = !filter.warnings;
    }
    ImGui::SameLine();
    if (chip((i18n::trf("Errors ({0})", {std::to_string(errors)}) + "##errors").c_str(), filter.errors)) filter.errors = !filter.errors;
}

// The lines that pass `filter` in a bordered child that fills the rest of the
// view; `to_end` scrolls to the last line once.
inline void log_lines(const char* id, const std::vector<LogLine>& lines, const LogFilter& filter, bool to_end) {
    std::vector<size_t> rows;
    rows.reserve(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        if (filter.shows(lines[i].level)) rows.push_back(i);
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    const bool open = ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleColor();
    if (open) {
        begin_mono();
        const float line = ImGui::GetTextLineHeight();
        const float gutter = line + ImGui::GetStyle().ItemInnerSpacing.x * 2;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const LogLine& l = lines[rows[static_cast<size_t>(i)]];
                const ImVec2 at = ImGui::GetCursorScreenPos();
                if (l.level == ANVIL_LOG_WARNING || l.level == ANVIL_LOG_ERROR) {
                    draw_glyph(ImGui::GetWindowDrawList(), at, line, l.level == ANVIL_LOG_ERROR ? Glyph::CannotLoad : Glyph::Attention);
                }
                ImGui::SetCursorScreenPos(ImVec2(at.x + gutter, at.y));
                ImGui::TextUnformatted(l.text.data(), l.text.data() + l.text.size());
            }
        }
        if (to_end) ImGui::SetScrollHereY(1.0f);
        end_mono();
    }
    ImGui::EndChild();
}

}  // namespace anvil::ui
