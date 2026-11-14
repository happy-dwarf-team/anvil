// Wrapped text that breaks Chinese, Japanese and Korean lines where those
// languages break them (src/ui/line_breaks.hpp). Text without such characters
// goes to ImGui's own wrapping unchanged. Header-only: the overlay does not
// link anvil_core.
#pragma once

#include "ui/line_breaks.hpp"

#include <imgui.h>

#include <cfloat>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::ui {

// TextWrapped("%s", text), breaking CJK lines as above.
inline void text_wrapped(std::string_view text) {
    if (!needs_own_breaks(text)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopTextWrapPos();
        return;
    }
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const float width = ImGui::GetContentRegionAvail().x;
    const std::vector<size_t> starts = wrap_starts(text, width, [&](const char* b, const char* e) {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, b, e).x;
    });
    std::string lines;
    lines.reserve(text.size() + starts.size());
    for (size_t k = 0; k < starts.size(); ++k) {
        size_t end = k + 1 < starts.size() ? starts[k + 1] : text.size();
        while (end > starts[k] && (text[end - 1] == ' ' || text[end - 1] == '\n')) --end;  // the spaces a line ends at
        if (k > 0) lines += '\n';
        lines.append(text.substr(starts[k], end - starts[k]));
    }
    ImGui::TextUnformatted(lines.data(), lines.data() + lines.size());
}

inline void text_wrapped(const std::string& text) { text_wrapped(std::string_view(text)); }
inline void text_wrapped(const char* text) { text_wrapped(std::string_view(text)); }

}  // namespace anvil::ui
