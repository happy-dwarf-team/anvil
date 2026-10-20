// The ImGui side of anvil-overlay: the notices and the panel (design §15).
// Everything here runs under Shared::mutex, in the EndScene hook or, for
// input, in the window procedure.
#include "notices.hpp"
#include "overlay.hpp"

#include <imgui.h>
#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace overlay {
namespace {

enum class Tab { None, Status, Mods, Settings, Crash, Log };

struct ShownNotice {
    Notice notice;
    ULONGLONG since = 0;
    bool closed = false;
};

struct Ui {
    // ImGui
    IDirect3DDevice9* dx9_device = nullptr;  // the DX9 backend's device
    HWND win32_window = nullptr;             // the Win32 backend's window
    bool was_visible = false;
    double font_scale = 0.0;  // the scale the font atlas was built for
    ImGuiStyle base_style;

    // Notices
    bool start_built = false;
    std::vector<ShownNotice> start;
    std::vector<Notice> session;

    // Panel caches
    ULONGLONG refreshed = 0;
    AnvilControlStatus status{};
    std::vector<PluginRow> plugins;
    std::vector<ModRow> mods;
    std::string confirm_approve;  // the mod plugin (id/mod) whose approval waits for a second click
    std::vector<std::string> warnings;
    bool crash_loaded = false;
    std::optional<CrashInfo> crash;
    std::string log;
    ULONGLONG log_refreshed = 0;
    bool log_follow = true;
    bool log_scroll = false;
    Tab requested_tab = Tab::None;

    // Settings editor
    std::string selected_plugin;
    std::map<std::string, std::string> text_edits;
    std::map<std::string, double> number_edits;
    std::string record_plugin;
    AnvilSetting record_setting = 0;
    std::string message;  // the result of the last action
    bool message_error = false;

    // Clipboard: written after the frame, outside the mutex; read on Ctrl+V.
    std::string copy;
    bool copy_pending = false;
    std::string paste;
};

Ui& ui() {
    static Ui u;
    return u;
}

const ImVec4 kRed(0.95f, 0.40f, 0.35f, 1.0f);
const ImVec4 kAmber(1.0f, 0.76f, 0.30f, 1.0f);
const ImVec4 kBlue(0.55f, 0.75f, 1.0f, 1.0f);
const ImVec4 kGreen(0.45f, 0.85f, 0.45f, 1.0f);

ImVec4 kind_color(NoticeKind kind) {
    return kind == NoticeKind::Error ? kRed : kind == NoticeKind::Warning ? kAmber : kBlue;
}

float scale() { return static_cast<float>(shared().scale.load()); }

void say(std::string text, bool error) {
    Ui& u = ui();
    u.message = std::move(text);
    u.message_error = error;
}

void copy_to_clipboard(const std::string& text) {
    Ui& u = ui();
    u.copy = text;
    u.copy_pending = true;
}

// ---- data ------------------------------------------------------------------------

void load_crash() {
    Ui& u = ui();
    if (u.crash_loaded) return;
    u.crash = shared().control.previous_crash();  // marks it as shown for later starts
    u.crash_loaded = true;
}

void refresh(bool force) {
    Ui& u = ui();
    const ULONGLONG now = GetTickCount64();
    if (!force && u.refreshed != 0 && now - u.refreshed < 1000) return;
    const ControlClient& control = shared().control;
    u.status = control.status();
    u.plugins = control.plugins();
    u.mods = control.mod_plugins();
    u.warnings = control.warnings();
    load_crash();
    u.refreshed = now;
}

void disable_plugin(const std::string& id) {
    const std::string error = shared().control.disable_next_start(id);
    if (error.empty()) {
        say(id + " is disabled from the next start (disabled_plugins in anvil.toml).", false);
    } else {
        say("Cannot disable " + id + ": " + error, true);
    }
    refresh(true);
    // The notices offer it no more.
    for (ShownNotice& shown : ui().start) {
        auto& actions = shown.notice.actions;
        actions.erase(std::remove_if(actions.begin(), actions.end(),
                                     [&](const NoticeAction& a) {
                                         return a.type == NoticeAction::Type::DisablePlugin && a.plugin == id &&
                                                error.empty();
                                     }),
                      actions.end());
        if (error.empty() && shown.notice.id == "crash") {
            shown.notice.lines.push_back(id + " is disabled from the next start.");
        }
    }
}

void build_start_notices(ULONGLONG now) {
    Ui& u = ui();
    const ControlClient& control = shared().control;
    const AnvilControlStatus status = control.status();
    if (!status.loading_done) {
        shared().mark_dirty();  // retry with the next frame
        return;
    }
    StartFacts facts;
    facts.safe_mode = status.safe_mode != 0;
    facts.unfinished_starts = status.unfinished_starts;
    facts.plugins = control.plugins();
    facts.mod_plugins = control.mod_plugins();
    load_crash();
    facts.crash = u.crash;
    for (Notice& n : start_notices(facts)) u.start.push_back({std::move(n), now, false});
    u.start_built = true;
    if (!u.start.empty()) {
        const std::string text = std::to_string(u.start.size()) + " notice(s) at the start";
        log_info(text.c_str());
    }
}

void build_session_notices() {
    const AnvilControlStatus status = shared().control.status();
    SessionFacts facts;
    facts.session = status.session != 0;
    facts.ironman = status.ironman != 0;
    facts.multiplayer = status.multiplayer != 0;
    facts.gameplay_active = status.gameplay_active;
    facts.gameplay_hash = status.gameplay_hash;
    ui().session = session_notices(facts);
}

bool notice_alive(const ShownNotice& n, ULONGLONG now) {
    if (n.closed || !shared().notifications.load()) return false;
    const int64_t seconds = shared().notification_seconds.load();
    return seconds <= 0 || now - n.since < static_cast<ULONGLONG>(seconds) * 1000;
}

void update_visible(ULONGLONG now) {
    Shared& s = shared();
    Ui& u = ui();
    bool any = !u.session.empty() || s.panel_open.load() || s.recording_key.load();
    for (const ShownNotice& n : u.start) any = any || notice_alive(n, now);
    s.visible.store(any, std::memory_order_release);
    if (!any) {
        s.want_mouse.store(false);
        s.want_keyboard.store(false);
        s.want_text.store(false);
    }
}

// ---- ImGui set-up ------------------------------------------------------------------

void set_clipboard(ImGuiContext*, const char* text) { copy_to_clipboard(text != nullptr ? text : ""); }
const char* get_clipboard(ImGuiContext*) { return ui().paste.c_str(); }

void build_fonts(double font_scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    // Latin with Latin Extended-A and general punctuation (the log's dashes).
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2000, 0x206F, 0};
    wchar_t windows[MAX_PATH] = {};
    ImFont* font = nullptr;
    if (GetWindowsDirectoryW(windows, MAX_PATH) != 0) {
        char path[MAX_PATH * 3] = {};
        const std::wstring file = std::wstring(windows) + L"\\Fonts\\segoeui.ttf";
        if (WideCharToMultiByte(CP_UTF8, 0, file.c_str(), -1, path, sizeof(path), nullptr, nullptr) > 0 &&
            GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ImFontConfig config;
            config.OversampleH = 2;
            font = io.Fonts->AddFontFromFileTTF(path, static_cast<float>(17.0 * font_scale), &config, ranges);
        }
    }
    if (font == nullptr) {
        ImFontConfig config;
        config.SizePixels = static_cast<float>(13.0 * font_scale);
        io.Fonts->AddFontDefault(&config);
    }
    io.Fonts->Build();
}

bool ensure_imgui(IDirect3DDevice9* device) {
    Shared& s = shared();
    Ui& u = ui();
    const HWND window = s.window.load();
    if (ImGui::GetCurrentContext() == nullptr) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // nothing written next to the game
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;  // the game keeps its cursor
        ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
        platform.Platform_SetClipboardTextFn = set_clipboard;
        platform.Platform_GetClipboardTextFn = get_clipboard;
        platform.Platform_SetImeDataFn = nullptr;  // IME calls could block on the window's thread
        platform.Platform_OpenInShellFn = nullptr;
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 6.0f;
        style.FrameRounding = 4.0f;
        style.WindowBorderSize = 1.0f;
        u.base_style = style;
        log_info("ImGui " IMGUI_VERSION " started");
    }
    if (u.win32_window != window) {
        if (u.win32_window != nullptr) ImGui_ImplWin32_Shutdown();
        u.win32_window = nullptr;
        if (window == nullptr || !ImGui_ImplWin32_Init(window)) return false;
        u.win32_window = window;
    }
    const double wanted = s.scale.load();
    if (u.font_scale != wanted) {
        if (u.dx9_device != nullptr) ImGui_ImplDX9_InvalidateDeviceObjects();
        build_fonts(wanted);
        ImGui::GetStyle() = u.base_style;
        ImGui::GetStyle().ScaleAllSizes(static_cast<float>(wanted));
        u.font_scale = wanted;
    }
    if (u.dx9_device != device) {
        if (u.dx9_device != nullptr) ImGui_ImplDX9_Shutdown();
        u.dx9_device = nullptr;
        if (!ImGui_ImplDX9_Init(device)) return false;
        u.dx9_device = device;
    }
    return true;
}

// EndScene may run for other render targets too; the overlay draws on the
// back buffer only.
bool targets_back_buffer(IDirect3DDevice9* device) {
    IDirect3DSurface9* target = nullptr;
    IDirect3DSurface9* back = nullptr;
    bool same = false;
    if (SUCCEEDED(device->GetRenderTarget(0, &target)) &&
        SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) {
        same = target == back;
    }
    if (target != nullptr) target->Release();
    if (back != nullptr) back->Release();
    return same;
}

// ---- widgets -------------------------------------------------------------------------

void help_marker(const char* text) {
    if (text == nullptr || *text == '\0') return;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ---- notices -------------------------------------------------------------------------

void run_action(const Notice& notice, const NoticeAction& action) {
    Shared& s = shared();
    if (action.type == NoticeAction::Type::DisablePlugin) {
        disable_plugin(action.plugin);
        return;
    }
    s.panel_open.store(true);
    ui().requested_tab = notice.id == "crash" || (notice.id == "safe-mode" && ui().crash) ? Tab::Crash
                         : notice.id == "approvals"                                ? Tab::Mods
                                                                                   : Tab::Status;
    refresh(true);
}

void draw_notice(const Notice& notice, bool* closed, float& y, int index) {
    const float k = scale();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = 440.0f * k;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12.0f * k, viewport->WorkPos.y + y),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    char name[64];
    std::snprintf(name, sizeof(name), "##notice-%s-%d", notice.id.c_str(), index);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin(name, nullptr, flags)) {
        const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const float button = ImGui::GetFrameHeight();
        ImGui::PushStyleColor(ImGuiCol_Text, kind_color(notice.kind));
        ImGui::PushTextWrapPos(closed != nullptr ? right - button : 0.0f);
        ImGui::TextWrapped("%s", notice.title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (closed != nullptr) {
            ImGui::SameLine(right - button);
            if (ImGui::SmallButton("x")) *closed = true;
        }
        for (const std::string& line : notice.lines) ImGui::TextWrapped("%s", line.c_str());
        bool first = true;
        for (const NoticeAction& action : notice.actions) {
            if (!first) ImGui::SameLine();
            first = false;
            if (ImGui::Button(action.label.c_str())) {
                run_action(notice, action);
                break;  // the actions may have changed
            }
        }
        y += ImGui::GetWindowHeight() + 8.0f * k;
    }
    ImGui::End();
}

void draw_notices(ULONGLONG now) {
    Ui& u = ui();
    float y = 12.0f * scale();
    int index = 0;
    for (const Notice& n : u.session) draw_notice(n, nullptr, y, index++);
    for (ShownNotice& n : u.start) {
        if (notice_alive(n, now)) draw_notice(n.notice, &n.closed, y, index++);
    }
}

// ---- the panel ---------------------------------------------------------------------------

const char* mode_text(const AnvilControlStatus& s) {
    if (s.ironman && s.multiplayer) return "ironman multiplayer game";
    if (s.ironman) return "ironman game";
    if (s.multiplayer) return "multiplayer game";
    return "single-player game";
}

void draw_status() {
    Ui& u = ui();
    const AnvilControlStatus& s = u.status;
    ImGui::Text("Anvil %s, game build %s", s.anvil_version ? s.anvil_version : "?",
                s.game_build ? s.game_build : "unknown");
    if (s.safe_mode) {
        ImGui::TextColored(kRed, "Safe mode: the last %u starts ended before the game was ready.", s.unfinished_starts);
    } else if (s.unfinished_starts > 0) {
        ImGui::TextColored(kAmber, "%u earlier start(s) ended before the game was ready.", s.unfinished_starts);
    }
    if (s.session) {
        if (s.gameplay_active > 0) {
            ImGui::Text("In a %s; %u gameplay plugin(s) act (%s).", mode_text(s), s.gameplay_active, s.gameplay_hash);
        } else {
            ImGui::Text("In a %s; no gameplay plugin acts.", mode_text(s));
        }
    }
    if (!s.loading_done) ImGui::TextDisabled("Plugins are still loading.");
    ImGui::Text("%u loaded, %u failed, %u skipped", s.loaded, s.failed, s.skipped);

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    const float warnings_height = u.warnings.empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing() * 6.0f;
    if (ImGui::BeginTable("plugins", 5, flags, ImVec2(0.0f, -warnings_height - ImGui::GetFrameHeightWithSpacing()))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Next start", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();
        for (const PluginRow& p : u.plugins) {
            ImGui::PushID(p.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.id.c_str());
            if (!p.name.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.version.c_str());
            ImGui::TableNextColumn();
            const ImVec4 color = p.state == "loaded" ? kGreen : p.state == "failed" ? kRed : kAmber;
            ImGui::TextColored(color, "%s", p.state.c_str());
            ImGui::TableNextColumn();
            std::string details = p.reason;
            // A skipped one's reason names its mod already.
            if (!p.mod.empty() && p.state != "skipped") details = "from mod '" + p.mod + "'" + (details.empty() ? "" : "; " + details);
            if (p.suspended) details += details.empty() ? "suspended in this game" : "; suspended in this game";
            if (p.gameplay) details += details.empty() ? "changes the game" : "; changes the game";
            ImGui::TextWrapped("%s", details.c_str());
            ImGui::TableNextColumn();
            if (p.disabled_next) {
                ImGui::TextDisabled("disabled");
            } else if (ImGui::SmallButton("Disable at next start")) {
                disable_plugin(p.id);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!u.warnings.empty()) {
        ImGui::Text("Warnings (%zu)", u.warnings.size());
        if (ImGui::BeginChild("warnings", ImVec2(0.0f, warnings_height - ImGui::GetFrameHeightWithSpacing()),
                              ImGuiChildFlags_Borders)) {
            for (const std::string& w : u.warnings) ImGui::TextWrapped("%s", w.c_str());
        }
        ImGui::EndChild();
    }
}

// ---- plugins in mods (design §14) ---------------------------------------------------

std::string mod_key(const ModRow& m) { return m.id + "/" + m.mod; }

std::string mod_source(const ModRow& m) {
    return "mod '" + m.mod + "' (" + (m.workshop_id.empty() ? std::string("local mod") : "Workshop " + m.workshop_id) + ")";
}

// After an approval or a revocation: the panel and the notice show what is left.
void mods_changed() {
    refresh(true);
    const bool any_waiting = std::any_of(ui().mods.begin(), ui().mods.end(), [](const ModRow& m) { return m.waiting(); });
    for (ShownNotice& shown : ui().start) {
        if (shown.notice.id == "approvals" && !any_waiting) shown.closed = true;
    }
}

void approve(const ModRow& m) {
    const std::string error = shared().control.approve_mod_plugin(m);
    if (error.empty()) {
        say(m.id + " " + m.version + " from " + mod_source(m) + " is approved; it loads from the next start.", false);
    } else {
        say("Cannot approve " + m.id + ": " + error, true);
    }
    ui().confirm_approve.clear();
    mods_changed();
}

void revoke(const ModRow& m) {
    const std::string error = shared().control.revoke_mod_plugin(m);
    if (error.empty()) {
        say("The approval of " + m.id + " is revoked; it does not load from the next start.", false);
    } else {
        say("Cannot revoke " + m.id + ": " + error, true);
    }
    mods_changed();
}

const char* state_text(const ModRow& m) {
    if (m.approved_next) return "approved for the next start";
    if (m.revoked_next) return "revoked from the next start";
    if (m.state == "approved") return m.loaded ? "approved, loaded" : "approved";
    if (m.state == "update-pending") return m.loaded ? "update waits; the approved version is loaded" : "update waits";
    if (m.state == "pending") return "waits for approval";
    if (m.state == "conflict") return "conflict";
    if (m.state == "shadowed") return "a local plugin wins";
    return "cannot be approved";
}

void draw_mods() {
    Ui& u = ui();
    if (u.mods.empty()) {
        ImGui::TextWrapped("No enabled mod in the playset has plugins (anvil/<id>/ in the mod's folder).");
        return;
    }
    ImGui::TextWrapped("Plugins from mods are native code: they run with the game's permissions and can do anything "
                       "your Windows account can. Approve only plugins whose authors you trust. An approval covers "
                       "these exact files (their hash); an update waits for a new approval. Approved plugins load "
                       "from the next start, from Anvil's own copy.");
    ImGui::Separator();
    if (ImGui::BeginChild("mods", ImVec2(0.0f, 0.0f))) {
        for (const ModRow& m : u.mods) {
            ImGui::PushID(mod_key(m).c_str());
            const ImVec4 color = m.approved_next || m.state == "approved" ? kGreen
                                 : m.state == "invalid" || m.revoked_next ? kRed
                                                                          : kAmber;
            const std::string header = m.id + (m.version.empty() ? "" : " " + m.version) + "  -  " + mod_source(m);
            const bool open = ImGui::CollapsingHeader(header.c_str(), m.waiting() ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            ImGui::SameLine();
            ImGui::TextColored(color, "%s", state_text(m));
            if (open) {
                ImGui::Indent();
                if (!m.name.empty()) ImGui::Text("Name: %s", m.name.c_str());
                if (!m.authors.empty()) ImGui::TextWrapped("Authors: %s", m.authors.c_str());
                if (!m.description.empty()) ImGui::TextWrapped("%s", m.description.c_str());
                if (m.gameplay) {
                    ImGui::TextColored(kAmber, "Changes the game (affects_gameplay): suspended in ironman and multiplayer "
                                               "unless you allow it.");
                } else {
                    ImGui::TextUnformatted("Does not change the game's simulation (affects_gameplay = false).");
                }
                ImGui::Text("Game addresses (symbols): %u", m.symbols);
                ImGui::TextWrapped("Hash: %s", m.hash.empty() ? "-" : m.hash.c_str());
                if (!m.repository.empty()) ImGui::TextWrapped("Source: %s", m.repository.c_str());
                if (!m.approved_hash.empty()) {
                    ImGui::TextWrapped("Approved: %s on %s (hash %s)", m.approved_version.c_str(),
                                       m.approved_date.substr(0, 10).c_str(), m.approved_hash.substr(0, 8).c_str());
                }
                ImGui::TextWrapped("%s", m.reason.c_str());
                if (m.can_approve && !m.approved_next) {
                    if (u.confirm_approve == mod_key(m)) {
                        ImGui::TextColored(kAmber, "Approve native code from %s? It loads from the next start.",
                                           mod_source(m).c_str());
                        if (ImGui::Button("Approve")) approve(m);
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel")) u.confirm_approve.clear();
                    } else if (ImGui::Button(m.state == "update-pending" ? "Approve the update..." : "Approve...")) {
                        u.confirm_approve = mod_key(m);
                    }
                }
                if (m.can_revoke) {
                    if (m.can_approve && !m.approved_next) ImGui::SameLine();
                    if (ImGui::Button("Revoke the approval")) revoke(m);
                }
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

std::string edit_key(const std::string& plugin, AnvilSetting setting) {
    return plugin + "/" + std::to_string(setting);
}

void apply_result(const std::string& error, const std::string& what) {
    if (error.empty()) {
        say(what, false);
    } else {
        say(error, true);
    }
}

void draw_setting(const std::string& plugin, AnvilSetting id, const AnvilControlSetting& info) {
    Shared& s = shared();
    Ui& u = ui();
    const ControlClient& control = s.control;
    const std::string current = control.setting_value(plugin, id, ANVIL_CONTROL_VALUE_CURRENT);
    const std::string saved = control.setting_value(plugin, id, ANVIL_CONTROL_VALUE_SAVED);
    const std::string fallback = control.setting_value(plugin, id, ANVIL_CONTROL_VALUE_DEFAULT);
    const std::string key = edit_key(plugin, id);
    const char* label = info.label;
    const float width = 220.0f * scale();
    ImGui::PushID(static_cast<int>(id));
    const auto applied = [&](const std::string& error) {
        apply_result(error, std::string(label) + (info.restart ? ": saved; applies at the next start" : ": applied"));
    };
    switch (info.type) {
    case ANVIL_CONTROL_SETTING_BOOL: {
        bool value = saved == "true";
        if (ImGui::Checkbox(label, &value)) {
            AnvilControlValue v{value ? 1 : 0, 0.0, nullptr};
            applied(control.set(plugin, id, v));
        }
        break;
    }
    case ANVIL_CONTROL_SETTING_INT:
    case ANVIL_CONTROL_SETTING_FLOAT: {
        const bool is_int = info.type == ANVIL_CONTROL_SETTING_INT;
        const auto it = u.number_edits.find(key);
        double value = it != u.number_edits.end() ? it->second : std::strtod(saved.c_str(), nullptr);
        ImGui::SetNextItemWidth(width);
        bool changed = false;
        if (is_int) {
            long long n = static_cast<long long>(value);
            const long long lo = static_cast<long long>(info.min), hi = static_cast<long long>(info.max);
            changed = info.has_min && info.has_max
                          ? ImGui::SliderScalar(label, ImGuiDataType_S64, &n, &lo, &hi, "%lld")
                          : ImGui::InputScalar(label, ImGuiDataType_S64, &n, nullptr, nullptr, "%lld");
            value = static_cast<double>(n);
        } else {
            changed = info.has_min && info.has_max
                          ? ImGui::SliderScalar(label, ImGuiDataType_Double, &value, &info.min, &info.max, "%.3g")
                          : ImGui::InputScalar(label, ImGuiDataType_Double, &value, nullptr, nullptr, "%.3g");
        }
        if (changed) u.number_edits[key] = value;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            AnvilControlValue v{static_cast<int64_t>(value), value, nullptr};
            applied(control.set(plugin, id, v));
            u.number_edits.erase(key);
        }
        break;
    }
    case ANVIL_CONTROL_SETTING_ENUM: {
        const auto options = control.setting_options(plugin, id);
        int selected = -1;
        for (size_t i = 0; i < options.size(); ++i) {
            if (options[i].first == saved) selected = static_cast<int>(i);
        }
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo(label, selected >= 0 ? options[static_cast<size_t>(selected)].second.c_str() : saved.c_str())) {
            for (size_t i = 0; i < options.size(); ++i) {
                if (ImGui::Selectable(options[i].second.c_str(), static_cast<int>(i) == selected)) {
                    AnvilControlValue v{static_cast<int64_t>(i), 0.0, nullptr};
                    applied(control.set(plugin, id, v));
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case ANVIL_CONTROL_SETTING_STRING: {
        auto it = u.text_edits.find(key);
        if (it == u.text_edits.end()) it = u.text_edits.emplace(key, saved).first;
        std::string& text = it->second;
        char buffer[1024];
        std::snprintf(buffer, sizeof(buffer), "%s", text.c_str());
        ImGui::SetNextItemWidth(width * 1.5f);
        const bool enter = ImGui::InputText(label, buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue);
        text = buffer;
        if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
            if (text != saved) applied(control.set_text(plugin, id, text));
            u.text_edits.erase(key);
        } else if (!ImGui::IsItemActive()) {
            text = saved;
        }
        break;
    }
    case ANVIL_CONTROL_SETTING_KEY: {
        const bool recording = s.recording_key.load() && u.record_plugin == plugin && u.record_setting == id;
        const std::string shown = recording ? "Press a key (Esc cancels)" : saved.empty() ? "(none)" : saved;
        if (ImGui::Button((shown + "##key").c_str(), ImVec2(width, 0.0f)) && !recording) {
            u.record_plugin = plugin;
            u.record_setting = id;
            s.recorded_key.store(-1);
            s.recording_key.store(true);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        if (!saved.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                AnvilControlValue v{0, 0.0, nullptr};
                applied(control.set(plugin, id, v));
            }
        }
        break;
    }
    default:
        ImGui::Text("%s: %s", label, saved.c_str());
        break;
    }
    help_marker(info.description);
    if (current != saved) {
        // restart = true: the saved value waits for the next start. Otherwise
        // an ANVIL_<ID>_<NAME> variable overrides it until the next change.
        ImGui::SameLine();
        ImGui::TextDisabled(info.restart ? "(now %s; applies at the next start)" : "(now %s, from the environment)",
                            current.c_str());
    }
    if (saved != fallback) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) applied(control.reset(plugin, id));
    }
    ImGui::PopID();
}

void draw_settings() {
    Ui& u = ui();
    const ControlClient& control = shared().control;
    std::vector<const PluginRow*> with_settings;
    for (const PluginRow& p : u.plugins) {
        if (p.settings > 0) with_settings.push_back(&p);
    }
    if (with_settings.empty()) {
        ImGui::TextDisabled("No plugin declares settings.");
        return;
    }
    if (u.selected_plugin.empty() ||
        std::none_of(with_settings.begin(), with_settings.end(), [&](const PluginRow* p) { return p->id == u.selected_plugin; })) {
        u.selected_plugin = with_settings.front()->id;
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.5f;
    if (ImGui::BeginChild("plugins", ImVec2(180.0f * scale(), -footer), ImGuiChildFlags_Borders)) {
        for (const PluginRow* p : with_settings) {
            if (ImGui::Selectable(p->id.c_str(), p->id == u.selected_plugin)) u.selected_plugin = p->id;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("editor", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
        const uint32_t count = control.setting_count(u.selected_plugin);
        std::string group;
        for (AnvilSetting id = 0; id < count; ++id) {
            const auto info = control.setting_info(u.selected_plugin, id);
            if (!info) continue;
            if (info->group != nullptr && group != info->group) {
                group = info->group;
                if (!group.empty()) ImGui::SeparatorText(group.c_str());
            }
            draw_setting(u.selected_plugin, id, *info);
        }
        for (const std::string& w : control.setting_warnings(u.selected_plugin)) ImGui::TextColored(kAmber, "%s", w.c_str());
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Values are saved in anvil\\data\\%s\\settings.toml.", u.selected_plugin.c_str());
}

void draw_crash() {
    Ui& u = ui();
    if (!u.crash) {
        ImGui::TextDisabled("No crash report from an earlier start.");
        return;
    }
    const CrashInfo& c = *u.crash;
    ImGui::Text("Report %s%s", c.name.c_str(), c.before_game_ready ? ", before the game was ready" : "");
    ImGui::TextWrapped("The crash was %s.", c.summary.empty() ? c.level.c_str() : c.summary.c_str());
    if (!c.plugins.empty()) {
        std::string list;
        for (const std::string& id : c.plugins) list += (list.empty() ? "" : ", ") + id;
        ImGui::TextWrapped("Plugins involved: %s", list.c_str());
    }
    if (!c.issues.empty()) {
        ImGui::TextWrapped("Report it at %s", c.issues.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy link")) copy_to_clipboard(c.issues);
    }
    if (ImGui::Button("Copy report")) {
        copy_to_clipboard(c.report);
        say("The report is in the clipboard. Nothing is uploaded.", false);
    }
    if (c.level == "in_plugin_code" || c.level == "in_plugin_change") {
        for (const std::string& id : c.plugins) {
            const auto row = std::find_if(u.plugins.begin(), u.plugins.end(), [&](const PluginRow& p) { return p.id == id; });
            if (row == u.plugins.end()) continue;
            ImGui::SameLine();
            if (row->disabled_next) {
                ImGui::TextDisabled("%s: disabled from the next start", id.c_str());
            } else if (ImGui::Button(("Disable " + id + " at the next start").c_str())) {
                disable_plugin(id);
            }
        }
    }
    if (ImGui::BeginChild("report", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::TextUnformatted(c.report.data(), c.report.data() + c.report.size());
    }
    ImGui::EndChild();
}

void draw_log() {
    Ui& u = ui();
    const ULONGLONG now = GetTickCount64();
    bool refresh_now = ImGui::Button("Refresh");
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &u.log_follow);
    ImGui::SameLine();
    if (ImGui::Button("Copy")) copy_to_clipboard(u.log);
    if (u.log_refreshed == 0 || refresh_now || (u.log_follow && now - u.log_refreshed > 2000)) {
        u.log = shared().control.log_tail(400);
        u.log_refreshed = now;
        u.log_scroll = u.log_follow || refresh_now;
    }
    if (ImGui::BeginChild("log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::TextUnformatted(u.log.data(), u.log.data() + u.log.size());
        if (u.log_scroll) {
            ImGui::SetScrollHereY(1.0f);
            u.log_scroll = false;
        }
    }
    ImGui::EndChild();
}

void draw_panel() {
    Shared& s = shared();
    Ui& u = ui();
    refresh(false);
    const float k = scale();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(820.0f * k, 540.0f * k), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("Anvil###anvil-panel", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        if (!u.message.empty()) {
            ImGui::TextColored(u.message_error ? kRed : kGreen, "%s", u.message.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("OK")) u.message.clear();
        }
        if (ImGui::BeginTabBar("tabs")) {
            const auto tab = [&](const char* name, Tab which, void (*body)()) {
                const ImGuiTabItemFlags flags = u.requested_tab == which ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(name, nullptr, flags)) {
                    body();
                    ImGui::EndTabItem();
                }
            };
            tab("Status", Tab::Status, draw_status);
            tab("Mods", Tab::Mods, draw_mods);
            tab("Settings", Tab::Settings, draw_settings);
            tab("Crash report", Tab::Crash, draw_crash);
            tab("Log", Tab::Log, draw_log);
            u.requested_tab = Tab::None;
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    if (!open) s.panel_open.store(false);
}

}  // namespace

// ---- the frame ---------------------------------------------------------------------------

void ui_frame(IDirect3DDevice9* device) {
    Shared& s = shared();
    Ui& u = ui();
    const ULONGLONG now = GetTickCount64();
    if (s.dirty.exchange(false, std::memory_order_acq_rel)) {
        if (s.start_ready.load() && !u.start_built) build_start_notices(now);
        if (s.session_changed.exchange(false)) build_session_notices();
        if (s.panel_open.load()) refresh(true);
    }
    // A key recorded for the settings editor.
    if (const int64_t recorded = s.recorded_key.exchange(-1); recorded >= 0) {
        if (recorded > 0 && !u.record_plugin.empty()) {
            AnvilControlValue v{recorded, 0.0, nullptr};
            apply_result(s.control.set(u.record_plugin, u.record_setting, v), "Key saved");
        }
        u.record_plugin.clear();
    }
    update_visible(now);
    if (!s.visible.load()) {
        u.was_visible = false;
        return;
    }
    if (!ensure_imgui(device) || !targets_back_buffer(device)) return;
    if (!u.was_visible) {
        // Input from while nothing was shown is stale.
        ImGui::GetIO().ClearEventsQueue();
        ImGui::GetIO().ClearInputKeys();
        u.was_visible = true;
    }
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_notices(now);
    if (s.panel_open.load()) draw_panel();
    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    const ImGuiIO& io = ImGui::GetIO();
    s.want_mouse.store(io.WantCaptureMouse);
    s.want_keyboard.store(s.panel_open.load() && io.WantCaptureKeyboard);
    s.want_text.store(io.WantTextInput);
    update_visible(now);
}

void ui_invalidate(IDirect3DDevice9* device) {
    Ui& u = ui();
    if (u.dx9_device == device) ImGui_ImplDX9_InvalidateDeviceObjects();
}

bool ui_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui::GetCurrentContext() == nullptr || ui().win32_window != hwnd) return false;
    return ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam) != 0;
}

void ui_flush_clipboard() {
    std::string text;
    {
        std::lock_guard lock(shared().mutex);
        Ui& u = ui();
        if (!u.copy_pending) return;
        text = std::move(u.copy);
        u.copy.clear();
        u.copy_pending = false;
    }
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (chars <= 0 || !OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(chars) * sizeof(wchar_t))) {
        if (auto* wide = static_cast<wchar_t*>(GlobalLock(memory))) {
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide, chars);
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void ui_capture_clipboard() {
    std::string text;
    if (OpenClipboard(nullptr)) {
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* wide = static_cast<const wchar_t*>(GlobalLock(data))) {
                const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
                if (bytes > 1) {
                    text.resize(static_cast<size_t>(bytes - 1));
                    WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), bytes, nullptr, nullptr);
                }
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
    }
    std::lock_guard lock(shared().mutex);
    ui().paste = std::move(text);
}

}  // namespace overlay
