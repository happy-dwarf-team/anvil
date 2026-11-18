// Ending a Dear ImGui frame that a C++ exception left half drawn.
//
// Dear ImGui is not exception safe: an exception thrown between NewFrame() and
// EndFrame() (a failed allocation in the drawing code) leaves its stacks as they
// were at the throw. The next NewFrame() empties only the window, popup, item
// flag and group stacks; a pushed colour, style variable, font, focus scope or
// BeginDisabled() stays pushed, and as NewFrame() stores those stack sizes as
// the frame's baseline, EndFrame()'s own recovery never pops them: the style
// stays changed until the game is restarted.
//
// end_failed_frame() pops back to the sizes stored at NewFrame() and ends the
// frame, without drawing it. A plain EndFrame() would pop them too, but ImGui's
// recovery reports every pop as a user error ("Missing End()", "Missing
// PopStyleColor()" in its debug log and error tooltip, an assert in a debug
// build); the exception was the plugin's, not a mismatched Begin/End, so the
// report is turned off for the call and set back afterwards.
#pragma once

#include <imgui.h>
#include <imgui_internal.h>

namespace anvil::ui {

// After an exception inside a frame (between NewFrame() and EndFrame()); does
// nothing outside one. Render() is not called: the half drawn frame is dropped.
inline void end_failed_frame() noexcept {
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr || !context->WithinFrameScope) return;
    ImGuiIO& io = context->IO;
    const bool recovery = io.ConfigErrorRecovery;
    const bool assert_on = io.ConfigErrorRecoveryEnableAssert;
    const bool log = io.ConfigErrorRecoveryEnableDebugLog;
    const bool tooltip = io.ConfigErrorRecoveryEnableTooltip;
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableDebugLog = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    ImGui::ErrorRecoveryTryToRecoverState(&context->StackSizesInNewFrame);
    ImGui::EndFrame();
    io.ConfigErrorRecovery = recovery;
    io.ConfigErrorRecoveryEnableAssert = assert_on;
    io.ConfigErrorRecoveryEnableDebugLog = log;
    io.ConfigErrorRecoveryEnableTooltip = tooltip;
}

}  // namespace anvil::ui
