#include "manager_app.hpp"

#include "core/anvil_toml_edit.hpp"
#include "core/crash_history.hpp"
#include "core/log.hpp"
#include "core/playset.hpp"
#include "core/semver.hpp"
#include "mods.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>

namespace anvil::manager {
namespace {

namespace fs = std::filesystem;

const ImVec4 kGood{0.40f, 0.78f, 0.45f, 1.0f};
const ImVec4 kWarn{0.96f, 0.72f, 0.26f, 1.0f};
const ImVec4 kBad{0.95f, 0.42f, 0.38f, 1.0f};

std::string u8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

void wrapped(const std::string& text) { ImGui::TextWrapped("%s", text.c_str()); }

void colored_wrapped(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    wrapped(text);
    ImGui::PopStyleColor();
}

void weak(const std::string& text) { ImGui::TextDisabled("%s", text.c_str()); }

void bullet(const std::string& text) {
    ImGui::Bullet();
    ImGui::SameLine();
    wrapped(text);
}

bool is_standard(const Package& package, const std::string& id) {
    return std::find(package.standard_plugins.begin(), package.standard_plugins.end(), id) != package.standard_plugins.end();
}

bool listed(const std::vector<std::string>& list, const std::string& id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

const char* step_text(Step::Kind kind) {
    switch (kind) {
    case Step::Kind::Write: return "write";
    case Step::Kind::CopyManager: return "copy the Manager to";
    case Step::Kind::Remove: return "remove";
    case Step::Kind::RemoveTree: return "remove the folder";
    case Step::Kind::RemoveIfEmpty: return "remove if empty";
    case Step::Kind::RemoveManager: return "remove";
    }
    return "";
}

// Modals: centred on every frame, a fixed width and the height of their contents.
void next_modal(float width_in_fonts) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * width_in_fonts, 0), ImGuiCond_Always);
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) out += (out.empty() ? "" : ", ") + item;
    return out;
}

}  // namespace

ManagerApp::ManagerApp(HWND window, std::optional<Package> package, std::string package_error, fs::path game)
    : window_(window), package_(std::move(package)), package_error_(std::move(package_error)), exe_(own_exe_path()) {
    settings_ = load_manager_settings();
    const fs::path steam = steam_root_from_registry();
    if (!steam.empty()) steam_games_ = find_steam_games(steam);
    if (game.empty() && runs_from(exe_, exe_.parent_path()) && inspect_game_folder(exe_.parent_path()).error.empty()) {
        game = exe_.parent_path();  // the copy in <EU4>\AnvilManager.exe
    }
    if (game.empty()) game = settings_.game_folder;
    if (game.empty() && steam_games_.size() == 1) game = steam_games_.front();
    if (!game.empty() && package_) choose_game(game);
    if (settings_.check_releases) release_pending_ = std::async(std::launch::async, fetch_latest_release);
}

void ManagerApp::choose_game(const fs::path& dir) {
    game_ = dir.lexically_normal();
    snapshot_.reset();
    editor_.reset();
    crash_selected_.clear();
    crash_text_.clear();
    log_text_.clear();
    running_checked_ = {};
    if (settings_.game_folder != game_) {
        settings_.game_folder = game_;
        std::string error;
        save_manager_settings(settings_, error);
    }
    refresh();
}

void ManagerApp::refresh() {
    if (game_.empty() || !package_) return;
    if (pending_.valid()) pending_.wait();
    pending_ = std::async(std::launch::async, [game = game_, package = &*package_, exe = exe_] {
        Snapshot snapshot;
        snapshot.folder = inspect_game_folder(game);
        if (!snapshot.folder.error.empty()) return snapshot;
        snapshot.installation = inspect_installation(*package, game, exe);
        std::error_code ec;
        if (fs::is_directory(game / "anvil", ec)) {
            cli::GameOptions options;
            options.game_dir = game;
            snapshot.report = cli::check_game(options);
        }
        snapshot.anvil_toml = read_anvil_settings(game / "anvil" / "anvil.toml");
        snapshot.crashes = crash_report_names(game / "anvil" / "crashes");
        return snapshot;
    });
}

void ManagerApp::poll() {
    using namespace std::chrono;
    if (pending_.valid() && pending_.wait_for(0s) == std::future_status::ready) snapshot_ = pending_.get();
    if (release_pending_.valid() && release_pending_.wait_for(0s) == std::future_status::ready) {
        release_ = release_pending_.get();
    }
    const auto now = steady_clock::now();
    if (!game_.empty() && now - running_checked_ > 1500ms) {
        running_checked_ = now;
        const bool was = running_;
        running_ = game_running(game_);
        if (was && !running_) refresh();  // the game may have written logs and crash reports
    }
}

bool ManagerApp::read_only() const { return running_ || pending_.valid(); }

void ManagerApp::draw() {
    poll();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Anvil Manager", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (!package_) {
        colored_wrapped(kBad, "This AnvilManager.exe is broken: " + package_error_ + ". Download it again from " +
                                  std::string(kReleasesUrl) + ".");
        ImGui::End();
        return;
    }
    draw_game_bar();
    ImGui::Separator();
    if (game_.empty() || (snapshot_ && !snapshot_->folder.error.empty())) {
        draw_first_run();
    } else if (!snapshot_) {
        weak("Reading the game folder...");
    } else if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Anvil")) {
            draw_anvil_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Plugins")) {
            draw_plugins_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Mods")) {
            draw_mods_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings", nullptr, show_settings_tab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            draw_settings_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Crashes")) {
            draw_crashes_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Log")) {
            draw_log_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("About")) {
            draw_about_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        show_settings_tab_ = false;
    }
    draw_modals();
    ImGui::End();
}

void ManagerApp::draw_game_bar() {
    ImGui::Text("Anvil Manager %s", ANVIL_VERSION);
    if (game_.empty()) return;
    ImGui::SameLine();
    weak("|");
    ImGui::SameLine();
    ImGui::TextUnformatted(u8(game_).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(running_);
    if (ImGui::SmallButton("Change...")) {
        if (const auto dir = pick_folder(window_, game_)) choose_game(*dir);
    }
    ImGui::EndDisabled();

    const float play_width = ImGui::CalcTextSize("Play EU4").x + ImGui::GetStyle().FramePadding.x * 4;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - play_width));
    ImGui::BeginDisabled(running_ || game_.empty());
    if (ImGui::Button("Play EU4", ImVec2(play_width, 0)) && !launch_game()) {
        show_message("Steam did not start", "Windows could not open steam://rungameid/236850. Is Steam installed?");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Starts the game through Steam");

    if (snapshot_ && snapshot_->folder.error.empty()) {
        if (snapshot_->folder.build.empty()) {
            colored_wrapped(kWarn, "eu4.exe is not a build Anvil knows (EU4 updated?): plugins that need game "
                                   "addresses are skipped, the others load.");
        } else {
            weak("eu4.exe: " + snapshot_->folder.build);
        }
    }
    if (running_) {
        colored_wrapped(kWarn, "EU4 is running from this folder. Close the game to change Anvil, plugins or settings; "
                               "until then the Manager only shows them.");
    }
}

void ManagerApp::draw_first_run() {
    if (snapshot_ && !snapshot_->folder.error.empty()) {
        colored_wrapped(kBad, u8(game_) + ": " + snapshot_->folder.error + ".");
        ImGui::Spacing();
    }
    wrapped("Choose the Europa Universalis IV folder: the one with eu4.exe.");
    ImGui::Spacing();
    if (!steam_games_.empty()) {
        weak("Steam has the game here:");
        for (const fs::path& game : steam_games_) {
            ImGui::PushID(u8(game).c_str());
            if (ImGui::Button("Use")) choose_game(game);
            ImGui::SameLine();
            ImGui::TextUnformatted(u8(game).c_str());
            ImGui::PopID();
        }
        ImGui::Spacing();
    } else {
        weak("Steam's libraries do not list the game.");
    }
    if (ImGui::Button("Choose the folder...")) {
        if (const auto dir = pick_folder(window_, game_)) choose_game(*dir);
    }
}

void ManagerApp::draw_anvil_tab() {
    const Installation& inst = *snapshot_->installation;
    const std::string& version = package_->version;
    const auto installed_version = Version::parse(inst.dll_version);
    const auto own_version = Version::parse(version);
    const bool newer_installed = installed_version && own_version && *own_version < *installed_version;

    ImGui::Spacing();
    switch (inst.dll) {
    case Installation::Dll::Missing:
        wrapped(inst.anvil_folder ? "Anvil is not installed (no version.dll). The anvil folder still has your plugins "
                                    "and settings: installing brings them back."
                                  : "Anvil is not installed. The game starts as usual (vanilla).");
        break;
    case Installation::Dll::Foreign:
        colored_wrapped(kWarn, "version.dll in the game folder is not Anvil's. Another mod or tool may use it: Anvil "
                               "replaces it only if you say so.");
        break;
    case Installation::Dll::Anvil:
        if (inst.dll_version != version) {
            wrapped("Anvil " + inst.dll_version + " is installed. This Manager carries Anvil " + version + ".");
            if (newer_installed) {
                colored_wrapped(kWarn, "The installed Anvil is newer: installing this one would go back to " + version +
                                           ". Use the Manager of the newer release instead.");
            }
        } else if (inst.intact()) {
            colored_wrapped(kGood, "Anvil " + version + " is installed and complete.");
        } else {
            colored_wrapped(kWarn, "Anvil " + version + " is installed, but some of its files differ from its package:");
        }
        break;
    }
    if (inst.manager_copy && !inst.manager_current && !runs_from(exe_, game_)) {
        weak("AnvilManager.exe in the game folder is another version than this one: installing replaces it.");
    }
    if (inst.dll == Installation::Dll::Anvil && inst.dll_version == version && !inst.intact()) {
        for (const auto& path : inst.missing) {
            if (path != kPlayerToml) bullet(path + " is missing");
        }
        for (const auto& path : inst.differing) bullet(path + " differs");
        for (const auto& path : inst.extra) bullet(path + " does not belong to Anvil");
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(read_only());
    const char* action = inst.dll != Installation::Dll::Anvil ? "Install Anvil"
                         : inst.dll_version != version         ? "Install this Anvil"
                                                               : "Repair";
    std::string label = std::string(action) + " " + version;
    if (inst.dll == Installation::Dll::Anvil && inst.dll_version == version) label = "Repair";
    if (inst.dll == Installation::Dll::Foreign) {
        ImGui::Checkbox("Replace the other version.dll", &replace_foreign_dll_);
    }
    const bool nothing = inst.intact() && inst.dll_version == version && inst.manager_current;
    ImGui::BeginDisabled(nothing || (inst.dll == Installation::Dll::Foreign && !replace_foreign_dll_));
    if (ImGui::Button(label.c_str())) start_plan(false);
    ImGui::EndDisabled();
    if (nothing) {
        ImGui::SameLine();
        weak("nothing to repair");
    }
    ImGui::EndDisabled();
    weak("Copies version.dll, the standard plugins (" + join(package_->standard_plugins) +
         ") and this Manager into the game folder. Your anvil.toml, plugins and their settings stay.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::BeginDisabled(read_only() || (inst.dll != Installation::Dll::Anvil && !inst.anvil_folder && !inst.manager_copy));
    ImGui::Checkbox("Also remove my plugins, their settings and data, crash reports and logs", &remove_everything_);
    if (ImGui::Button("Uninstall Anvil")) start_plan(true);
    ImGui::EndDisabled();
    weak("Without version.dll the game starts vanilla at once.");

    if (!settings_.asked_releases) {
        ImGui::Spacing();
        ImGui::Separator();
        wrapped("Should the Manager ask GitHub for new Anvil releases when it starts? It only shows a link; nothing "
                "is downloaded. You can change this in About.");
        const auto answer = [&](bool yes) {
            settings_.asked_releases = true;
            settings_.check_releases = yes;
            std::string error;
            save_manager_settings(settings_, error);
            if (yes && !release_pending_.valid()) release_pending_ = std::async(std::launch::async, fetch_latest_release);
        };
        if (ImGui::Button("Yes, check for releases")) answer(true);
        ImGui::SameLine();
        if (ImGui::Button("No")) answer(false);
    }
    if (release_ && release_->release && newer_release(release_->release->tag, ANVIL_VERSION)) {
        ImGui::Spacing();
        colored_wrapped(kGood, "Anvil " + release_->release->tag + " is available.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Open its page")) open_url(release_->release->url);
    }
}

void ManagerApp::draw_plugins_tab() {
    const auto& report = snapshot_->report;
    ImGui::Spacing();
    ImGui::BeginDisabled(read_only() || !report || !report->cannot_check.empty());
    if (ImGui::Button("Install a plugin from a zip...")) {
        if (const auto file = pick_zip(window_)) open_zip(*file);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open the plugins folder")) open_folder(game_ / "anvil" / "plugins");
    ImGui::SameLine();
    ImGui::BeginDisabled(pending_.valid());
    if (ImGui::Button("Check again")) refresh();
    ImGui::EndDisabled();
    weak("You can also drop a plugin zip on this window.");
    ImGui::Spacing();

    if (!report) {
        wrapped("Anvil is not installed in this folder: no plugins load.");
        return;
    }
    if (!report->cannot_check.empty()) {
        colored_wrapped(kBad, "Cannot check the plugins: " + report->cannot_check);
        return;
    }
    for (const cli::Problem& problem : report->problems) {
        // dlc_load.json and trust.toml are shown on the Mods tab.
        if (u8(fs::path(widen(problem.file)).filename()) == "anvil.toml") colored_wrapped(kBad, "anvil.toml: " + problem.message);
    }
    if (!report->mod_plugins.empty()) {
        size_t waiting = 0;
        for (const cli::GameModPlugin& p : report->mod_plugins) {
            waiting += p.state == "pending" || p.state == "update-pending" || p.state == "conflict" ? 1 : 0;
        }
        const std::string text = std::to_string(report->mod_plugins.size()) + " plugin(s) from enabled mods" +
                                 (waiting > 0 ? ", " + std::to_string(waiting) + " waiting for your approval" : std::string()) +
                                 ": see the Mods tab.";
        if (waiting > 0) colored_wrapped(kWarn, text);
        else weak(text);
    }
    if (!report->enabled) colored_wrapped(kWarn, "anvil.toml says enabled = false: no plugin loads.");
    if (snapshot_->installation && !snapshot_->installation->installed()) {
        colored_wrapped(kWarn, "version.dll is not Anvil's or missing: nothing below loads until Anvil is installed.");
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("plugins", 5, flags)) {
        ImGui::TableSetupColumn("Load", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const cli::GamePlugin& plugin : report->plugins) {
            const auto& manifest = plugin.report.manifest;
            const std::string id = manifest ? manifest->id : plugin.folder;
            ImGui::PushID(plugin.folder.c_str());
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            bool load = !listed(report->disabled_plugins, id);
            ImGui::BeginDisabled(read_only() || !manifest);
            if (ImGui::Checkbox("##load", &load)) edit_list("disabled_plugins", id, !load);
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(manifest && !manifest->name.empty() ? manifest->name.c_str() : plugin.folder.c_str());
            if (manifest && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
                ImGui::TextUnformatted(("id " + manifest->id + ", folder " + plugin.folder).c_str());
                if (!manifest->authors.empty()) ImGui::TextUnformatted(("by " + join(manifest->authors)).c_str());
                if (!manifest->description.empty()) ImGui::TextUnformatted(manifest->description.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            if (is_standard(*package_, id)) {
                ImGui::SameLine();
                weak("(comes with Anvil)");
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(manifest ? manifest->version.to_string().c_str() : "-");

            ImGui::TableNextColumn();
            const auto skipped = std::find_if(report->skipped.begin(), report->skipped.end(),
                                              [&](const cli::SkippedPlugin& s) { return s.folder == plugin.folder; });
            if (plugin.status == "loaded") {
                ImGui::TextColored(kGood, "loads");
            } else if (plugin.status == "disabled") {
                weak("disabled");
            } else {
                colored_wrapped(kBad, "skipped" + (skipped != report->skipped.end() ? ": " + skipped->reason : std::string()));
            }
            for (const cli::Problem& problem : plugin.report.problems) {
                ImGui::PushStyleColor(ImGuiCol_Text, plugin.disabled ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : kWarn);
                wrapped(problem.message);
                ImGui::PopStyleColor();
            }
            if (manifest && manifest->affects_gameplay) {
                weak("Changes the game: kept out of ironman and multiplayer unless allowed.");
                ImGui::BeginDisabled(read_only());
                if (manifest->allow_ironman == Allow::OptIn) {
                    bool allow = listed(snapshot_->anvil_toml.allow_ironman, id);
                    if (ImGui::Checkbox("Allow in ironman", &allow)) edit_list("allow_ironman", id, allow);
                    ImGui::SameLine();
                }
                if (manifest->allow_multiplayer == Allow::OptIn) {
                    bool allow = listed(snapshot_->anvil_toml.allow_multiplayer, id);
                    if (ImGui::Checkbox("Allow in multiplayer", &allow)) edit_list("allow_multiplayer", id, allow);
                }
                ImGui::EndDisabled();
            }

            ImGui::TableNextColumn();
            if (manifest && !manifest->settings.empty() && ImGui::SmallButton("Settings")) open_settings(plugin);
            if (!is_standard(*package_, id)) {
                ImGui::BeginDisabled(read_only());
                if (ImGui::SmallButton("Remove")) ImGui::OpenPopup("remove");
                ImGui::EndDisabled();
            }
            if (ImGui::BeginPopup("remove")) {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22);
                ImGui::TextUnformatted(("Remove the folder anvil/plugins/" + plugin.folder +
                                        "? Its settings and data in anvil/data stay.").c_str());
                ImGui::PopTextWrapPos();
                if (ImGui::Button("Remove")) {
                    std::string error;
                    if (!remove_plugin_folder(game_, plugin.folder, package_->standard_plugins, error)) {
                        failed("The plugin was not removed", error, false);
                    }
                    refresh();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (report->plugins.empty()) weak("No plugins are installed.");

    if (!report->load_order.empty()) {
        ImGui::Spacing();
        weak("Load order:");
        std::string order;
        for (const cli::LoadedPlugin& p : report->load_order) order += (order.empty() ? "" : " > ") + p.id + " " + p.version +
                                                                    (p.mod.empty() ? "" : " (mod '" + p.mod + "')");
        wrapped(order);
    }
    for (const cli::OverlapWarning& warning : report->warnings) colored_wrapped(kWarn, warning.message);
}

namespace {

std::string mod_source(const cli::GameModPlugin& p) {
    return "mod '" + p.mod + "' (" + (p.workshop_id.empty() ? std::string("local mod") : "Workshop " + p.workshop_id) + ")";
}

// What a plugin from a mod waits for, or does, in the player's words.
std::string mod_state_text(const cli::GameModPlugin& p) {
    if (p.state == "approved") return p.status == "loaded" ? "approved: loads" : "approved, but " + p.skip_reason;
    if (p.state == "update-pending") return "an update waits for approval; the approved " + p.approved_version + " loads";
    if (p.state == "pending") return "waits for approval: does not load";
    if (p.state == "conflict") return "conflict: does not load";
    if (p.state == "shadowed") return "a plugin in anvil/plugins has this id and loads instead";
    return "cannot be approved: does not load";
}

}  // namespace

void ManagerApp::draw_mods_tab() {
    const auto& report = snapshot_->report;
    ImGui::Spacing();
    ImGui::BeginDisabled(pending_.valid());
    if (ImGui::Button("Check again")) refresh();
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (!report) {
        wrapped("Anvil is not installed in this folder: no plugins load.");
        return;
    }
    wrapped("Mods from the Workshop or your mod folder can carry plugins in their anvil/<id>/ folder. Only the mods "
            "enabled in the playset the game starts with count (" + std::to_string(report->enabled_mods) +
            " enabled; the launcher changes the playset). A plugin from a mod loads only after you approve its exact "
            "files; an update waits for a new approval while the approved version keeps loading.");
    if (!report->playset_found) weak("No dlc_load.json in " + report->user_dir + ": no mods are enabled.");
    for (const std::string& w : report->mod_warnings) colored_wrapped(kWarn, w);
    for (const cli::Problem& problem : report->problems) {
        const std::string file = u8(fs::path(widen(problem.file)).filename());
        if (file != "anvil.toml") colored_wrapped(kBad, file + ": " + problem.message);
    }
    if (report->mod_plugins.empty()) {
        ImGui::Spacing();
        weak("No enabled mod has plugins.");
        return;
    }
    if (read_only() && running_) colored_wrapped(kWarn, "The game is running: approvals can be changed after it closes.");
    ImGui::Spacing();
    for (const cli::GameModPlugin& p : report->mod_plugins) {
        ImGui::PushID((p.id + "/" + p.descriptor).c_str());
        ImGui::Separator();
        const auto& m = p.report.manifest;
        ImGui::Text("%s %s", m && !m->name.empty() ? m->name.c_str() : p.id.c_str(), p.version.c_str());
        ImGui::SameLine();
        weak("id " + p.id + ", " + mod_source(p));
        const bool good = p.state == "approved" && p.status == "loaded";
        const bool waits = p.state == "pending" || p.state == "update-pending" || p.state == "conflict";
        colored_wrapped(good ? kGood : waits ? kWarn : p.state == "shadowed" ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : kBad,
                        mod_state_text(p));
        weak(p.reason);
        if (!p.others.empty()) weak("Other mods with this id: " + join(p.others));
        for (const cli::Problem& problem : p.report.problems) colored_wrapped(kWarn, problem.message);
        const bool approved_here = !p.approved_hash.empty() && p.approved_hash == p.hash;
        const bool can_approve = p.state != "invalid" && p.state != "shadowed" && !p.hash.empty() && m && !approved_here;
        ImGui::BeginDisabled(read_only());
        if (can_approve && ImGui::Button(p.state == "update-pending" ? "Approve the update..." : "Approve...")) {
            approve_ = p;
            open_approve_ = true;
        }
        if (!p.approved_hash.empty()) {
            if (can_approve) ImGui::SameLine();
            if (ImGui::Button("Revoke the approval")) {
                std::string error;
                if (!revoke_shown_plugin(game_, p, error)) failed("The approval was not revoked", error, false);
                refresh();
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
}

void ManagerApp::draw_approve_modal() {
    next_modal(40);
    if (!ImGui::BeginPopupModal("Approve a plugin from a mod###approve", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    if (!approve_ || !snapshot_ || !snapshot_->report) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const cli::GameModPlugin& p = *approve_;
    const Manifest& m = *p.report.manifest;
    ImGui::Text("%s %s", m.name.empty() ? m.id.c_str() : m.name.c_str(), p.version.c_str());
    weak("id " + m.id + (m.authors.empty() ? std::string() : ", by " + join(m.authors)));
    weak("from " + mod_source(p));
    if (!m.description.empty()) wrapped(m.description);
    if (!m.repository.empty() && m.repository.starts_with("https://")) {
        if (ImGui::SmallButton("Open its repository")) open_url(m.repository);
    }
    ImGui::Spacing();
    colored_wrapped(kWarn, "A plugin is native code: it runs inside the game with your rights, like any program. "
                           "Approve plugins only from authors you trust.");
    if (m.affects_gameplay) {
        bullet("It changes the game. Anvil keeps it out of ironman and multiplayer games unless you allow it there.");
    } else {
        bullet("It says it does not change the game (affects_gameplay = false).");
    }
    bullet(p.report.has_symbols ? std::to_string(p.report.symbols) + " game address(es) declared in symbols.toml"
                                : "It declares no game addresses (no symbols.toml).");
    bullet("Hash of its files: " + p.hash);
    if (p.state == "update-pending") bullet("It replaces the approved " + p.approved_version + " from the next start.");
    if (p.state == "conflict") bullet("Other enabled mods have a plugin with this id; this approval picks this one.");
    if (p.status == "skipped" && !p.report.skip_reason.empty()) colored_wrapped(kBad, "Anvil would still skip it: " + p.report.skip_reason);
    ImGui::Spacing();
    if (ImGui::Button("Approve")) {
        std::string error;
        if (!approve_shown_plugin(game_, eu4_user_dir(game_), *snapshot_->report, p, error)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            approve_.reset();
            failed("The plugin was not approved", error, false);
            refresh();
            return;
        }
        approve_.reset();
        refresh();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        approve_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ManagerApp::open_settings(const cli::GamePlugin& plugin) {
    const Manifest& manifest = *plugin.report.manifest;
    SettingsEditor editor;
    editor.plugin = manifest.id;
    editor.player_file = game_ / "anvil" / "data" / widen(manifest.id) / "settings.toml";
    const SettingsFileResult schema =
        load_settings(game_ / "anvil" / "plugins" / widen(plugin.folder) / fs::path(widen(manifest.settings)));
    if (!schema.file) {
        editor.error = schema.error;
    } else {
        editor.schema = *schema.file;
        PlayerSettings player = load_player_settings(editor.schema, editor.player_file);
        editor.warnings = std::move(player.warnings);
        for (const Setting& setting : editor.schema.settings) {
            const SettingValue value = player.values[setting.id].value_or(setting.default_value);
            editor.values.push_back(value);
            editor.texts.push_back(setting.type == SettingType::Key      ? key_name(value.integer)
                                   : setting.type == SettingType::String ? value.text
                                                                          : std::string());
        }
    }
    editor_ = std::move(editor);
    show_settings_tab_ = true;
}

void ManagerApp::draw_settings_tab() {
    const auto& report = snapshot_->report;
    ImGui::Spacing();
    if (!report || report->plugins.empty()) {
        wrapped("No installed plugin has settings.");
        return;
    }
    const std::string current = editor_ ? editor_->plugin : std::string("Choose a plugin");
    if (ImGui::BeginCombo("Plugin", current.c_str())) {
        for (const cli::GamePlugin& plugin : report->plugins) {
            const auto& manifest = plugin.report.manifest;
            if (!manifest || manifest->settings.empty()) continue;
            if (ImGui::Selectable(manifest->id.c_str(), editor_ && editor_->plugin == manifest->id)) open_settings(plugin);
        }
        ImGui::EndCombo();
    }
    if (!editor_) {
        weak("Plugins with a settings.toml can be set up here. The values go to anvil/data/<plugin>/settings.toml "
             "and apply when the game starts.");
        return;
    }
    SettingsEditor& editor = *editor_;
    if (!editor.error.empty()) {
        colored_wrapped(kBad, editor.error);
        return;
    }
    for (const std::string& warning : editor.warnings) colored_wrapped(kWarn, "Ignored in your file: " + warning);
    ImGui::Spacing();
    ImGui::BeginDisabled(read_only());
    std::string group;
    for (const Setting& setting : editor.schema.settings) {
        SettingValue& value = editor.values[setting.id];
        std::string& text = editor.texts[setting.id];
        if (setting.group != group) {
            group = setting.group;
            ImGui::Spacing();
            ImGui::SeparatorText(group.c_str());
        }
        ImGui::PushID(static_cast<int>(setting.id));
        const std::string label = setting.label.empty() ? setting.name : setting.label;
        bool changed = false;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        switch (setting.type) {
        case SettingType::Bool: {
            bool on = value.integer != 0;
            if (ImGui::Checkbox(label.c_str(), &on)) {
                value.integer = on ? 1 : 0;
                changed = true;
            }
            break;
        }
        case SettingType::Int: {
            const int64_t step = setting.step ? setting.step->integer : 1;
            changed = ImGui::InputScalar(label.c_str(), ImGuiDataType_S64, &value.integer, &step);
            if (setting.min) value.integer = std::max(value.integer, setting.min->integer);
            if (setting.max) value.integer = std::min(value.integer, setting.max->integer);
            break;
        }
        case SettingType::Float: {
            const double step = setting.step ? setting.step->real : 0.0;
            changed = ImGui::InputDouble(label.c_str(), &value.real, step, step * 10, "%.4g");
            if (setting.min) value.real = std::max(value.real, setting.min->real);
            if (setting.max) value.real = std::min(value.real, setting.max->real);
            break;
        }
        case SettingType::Enum: {
            const auto index = static_cast<size_t>(value.integer);
            const std::string shown = index < setting.options.size() ? setting.options[index].label : "?";
            if (ImGui::BeginCombo(label.c_str(), shown.c_str())) {
                for (size_t i = 0; i < setting.options.size(); ++i) {
                    if (ImGui::Selectable(setting.options[i].label.c_str(), i == index)) {
                        value.integer = static_cast<int64_t>(i);
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case SettingType::String:
            if (ImGui::InputText(label.c_str(), &text)) {
                value.text = text;
                changed = true;
            }
            break;
        case SettingType::Key:
            if (ImGui::InputTextWithHint(label.c_str(), "no key", &text)) {
                if (const auto code = parse_key(text)) value.integer = *code;
                changed = true;
            }
            if (!parse_key(text)) {
                ImGui::SameLine();
                ImGui::TextColored(kBad, "not a key (e.g. Ctrl+Shift+F12)");
            }
            break;
        }
        if (!(value == setting.default_value)) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Default")) {
                value = setting.default_value;
                text = setting.type == SettingType::Key ? key_name(value.integer) : value.text;
                changed = true;
            }
        }
        if (!setting.description.empty()) {
            ImGui::Indent();
            weak(setting.description);
            ImGui::Unindent();
        }
        editor.dirty = editor.dirty || changed;
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(!editor.dirty);
    if (ImGui::Button("Save")) save_settings();
    ImGui::SameLine();
    if (ImGui::Button("Undo changes")) {
        const auto& plugins = snapshot_->report->plugins;
        const auto it = std::find_if(plugins.begin(), plugins.end(), [&](const cli::GamePlugin& p) {
            return p.report.manifest && p.report.manifest->id == editor.plugin;
        });
        if (it != plugins.end()) open_settings(*it);
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    weak("Saved to " + u8(editor_ ? editor_->player_file : fs::path()) + "; values apply when the game starts.");
}

void ManagerApp::save_settings() {
    SettingsEditor& editor = *editor_;
    for (const Setting& setting : editor.schema.settings) {
        if (setting.type == SettingType::Key && !parse_key(editor.texts[setting.id])) {
            show_message("Not saved", (setting.label.empty() ? setting.name : setting.label) + ": '" +
                                          editor.texts[setting.id] + "' is not a key.");
            return;
        }
        if (const std::string why = check_setting_value(setting, editor.values[setting.id]); !why.empty()) {
            show_message("Not saved", (setting.label.empty() ? setting.name : setting.label) + ": " + why);
            return;
        }
    }
    std::string error;
    if (!write_player_settings(editor.schema, editor.values, editor.player_file, error)) {
        failed("The settings were not saved", error, error.find("error 5)") != std::string::npos);
        return;
    }
    editor.dirty = false;
    editor.warnings.clear();
}

void ManagerApp::draw_crashes_tab() {
    ImGui::Spacing();
    const fs::path folder = game_ / "anvil" / "crashes";
    if (ImGui::Button("Open the crashes folder")) open_folder(folder);
    ImGui::Spacing();
    if (snapshot_->crashes.empty()) {
        wrapped("No crash reports. When the game crashes with Anvil, a report is written here (the last 10 are kept).");
        return;
    }
    ImGui::BeginChild("list", ImVec2(ImGui::GetFontSize() * 14, 0), ImGuiChildFlags_Borders);
    for (const std::string& name : snapshot_->crashes) {
        if (ImGui::Selectable(name.c_str(), name == crash_selected_)) {
            crash_selected_ = name;
            crash_text_ = read_text_file(folder / widen(name + ".txt"), 1 << 20).value_or("(the report's text cannot be read)");
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("report");
    if (crash_selected_.empty()) {
        weak("Choose a report.");
    } else if (const auto crash = load_crash_summary(folder, crash_selected_)) {
        wrapped(crash->time + ": " + (crash->summary.empty() ? std::string("a crash") : crash->summary));
        if (!crash->plugins.empty()) wrapped("Plugins involved: " + join(crash->plugins));
        if (crash->before_game_ready) weak("Before the game was ready.");
        if (ImGui::Button("Copy the report")) copy_to_clipboard(window_, crash_text_);
        if (!crash->issues.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Report it to the plugin's author")) open_url(crash->issues);
        }
        ImGui::InputTextMultiline("##text", &crash_text_, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
    } else {
        weak("The report cannot be read.");
    }
    ImGui::EndChild();
}

void ManagerApp::draw_log_tab() {
    ImGui::Spacing();
    bool& previous = log_previous_;
    const fs::path file = game_ / "anvil" / (previous ? "anvil.prev.log" : "anvil.log");
    bool reload = log_text_.empty();
    if (ImGui::RadioButton("Last start", !previous)) {
        previous = false;
        reload = true;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("The start before", previous)) {
        previous = true;
        reload = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) reload = true;
    ImGui::SameLine();
    if (ImGui::Button("Copy")) copy_to_clipboard(window_, log_text_);
    ImGui::SameLine();
    if (ImGui::Button("Open the folder")) open_folder(game_ / "anvil");
    if (reload) {
        log_text_ = read_text_tail(file, 2000, 512 << 10);
        if (log_text_.empty()) log_text_ = "(" + u8(file.filename()) + " is empty or missing: Anvil writes it when the game starts)";
    }
    ImGui::InputTextMultiline("##log", &log_text_, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
}

void ManagerApp::draw_about_tab() {
    ImGui::Spacing();
    wrapped("Anvil Manager " + std::string(ANVIL_VERSION) + " installs Anvil " + package_->version +
            ", the native plugin loader for Europa Universalis IV 1.37.5, and manages its plugins.");
    weak("Unofficial community project, not affiliated with Paradox Interactive. MIT License.");
    if (ImGui::Button("Anvil on GitHub")) open_url("https://github.com/happy-dwarf-team/anvil");
    ImGui::Spacing();
    ImGui::SeparatorText("New releases");
    bool check = settings_.check_releases;
    if (ImGui::Checkbox("Ask GitHub for the latest release when the Manager starts", &check)) {
        settings_.check_releases = check;
        settings_.asked_releases = true;
        std::string error;
        save_manager_settings(settings_, error);
    }
    weak("Only a link is shown; nothing is downloaded. To update Anvil, run the Manager of the new release.");
    ImGui::BeginDisabled(release_pending_.valid());
    if (ImGui::Button("Check now")) release_pending_ = std::async(std::launch::async, fetch_latest_release);
    ImGui::EndDisabled();
    if (release_pending_.valid()) {
        ImGui::SameLine();
        weak("asking GitHub...");
    } else if (release_) {
        ImGui::SameLine();
        if (!release_->release) {
            ImGui::TextColored(kWarn, "%s", release_->error.c_str());
        } else if (newer_release(release_->release->tag, ANVIL_VERSION)) {
            ImGui::TextColored(kGood, "Anvil %s is available.", release_->release->tag.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Open its page")) open_url(release_->release->url);
        } else {
            ImGui::TextUnformatted(("You have the latest release (" + release_->release->tag + ").").c_str());
        }
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Third-party code");
    weak("Dear ImGui, miniz, toml++ and Zydis (MIT License). Their license texts are in anvil/docs/third-party after "
         "installing.");
}

void ManagerApp::start_plan(bool uninstall) {
    const Installation& inst = *snapshot_->installation;
    PendingPlan pending;
    pending.uninstall = uninstall;
    pending.plan = uninstall ? plan_uninstall(*package_, inst, remove_everything_)
                             : plan_install(*package_, inst, game_, exe_, replace_foreign_dll_);
    plan_ = std::move(pending);
    open_plan_ = true;
}

void ManagerApp::run_plan() {
    const PendingPlan pending = std::move(*plan_);
    plan_.reset();
    if (game_running(game_)) {
        running_ = true;
        show_message("Nothing changed", "EU4 is running from this folder. Close it first.");
        return;
    }
    const ApplyResult result = apply_plan(*package_, pending.plan, game_, exe_);
    refresh();
    if (!result.ok) {
        failed(pending.uninstall ? "Anvil was not fully uninstalled" : "Anvil was not fully installed",
               result.error + (result.done > 0 ? "\n\nThe steps before it are done; run it again once the cause is fixed."
                                               : std::string()),
               result.access_denied);
        return;
    }
    if (!pending.uninstall) {
        show_message("Done", "Anvil " + package_->version + " is installed. Start the game to load your plugins.");
    } else if (result.manager_left) {
        const bool scheduled = remove_after_exit(game_ / kManagerExe);
        show_message("Done", scheduled ? "Anvil is uninstalled. The Manager closes now and removes its copy in the game "
                                         "folder."
                                       : "Anvil is uninstalled. Delete AnvilManager.exe in the game folder yourself once "
                                         "the Manager is closed.");
        close_after_message_ = true;
    } else {
        show_message("Done", "Anvil is uninstalled. The game starts vanilla.");
    }
}

void ManagerApp::open_zip(const fs::path& file) {
    if (read_only() || !snapshot_ || !snapshot_->report) {
        show_message("Not now", running_ ? "Close the game first." : "Install Anvil first, then its plugins.");
        return;
    }
    std::string error;
    auto zip = ZipArchive::open_file(file, error);
    if (!zip) {
        show_message("Not a plugin zip", u8(file.filename()) + ": " + error);
        return;
    }
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    zip_check_ = check_plugin_zip(*zip, game_, temp, package_->standard_plugins);
    zip_ = std::move(zip);
    zip_name_ = u8(file.filename());
    zip_anyway_ = false;
    open_zip_ = true;
}

void ManagerApp::install_zip() {
    if (game_running(game_)) {
        running_ = true;
        show_message("Nothing changed", "EU4 is running from this folder. Close it first.");
        return;
    }
    const PluginInstallResult result = install_plugin_zip(*zip_, *zip_check_, game_);
    const std::string id = zip_check_->id;
    zip_.reset();
    zip_check_.reset();
    refresh();
    if (!result.ok) {
        failed("The plugin was not installed", result.error, result.access_denied);
        return;
    }
    show_message("Done", id + " is installed in anvil/plugins/" + u8(result.folder.filename()) +
                             ". It loads when the game starts.");
}

void ManagerApp::dropped(const fs::path& file) {
    if (_wcsicmp(file.extension().c_str(), L".zip") != 0) {
        show_message("Not a zip", "Drop a plugin's zip (<id>-<version>.zip) here to install it.");
        return;
    }
    open_zip(file);
}

void ManagerApp::edit_list(const std::string& key, const std::string& id, bool listed_now) {
    std::string error;
    if (!set_plugin_listed(game_ / "anvil" / "anvil.toml", key, id, listed_now, error)) {
        failed("anvil.toml was not changed", error, error.find("error 5)") != std::string::npos);
    }
    refresh();
}

void ManagerApp::show_message(std::string title, std::string text) {
    message_title_ = std::move(title);
    message_text_ = std::move(text);
    offer_elevation_ = false;
    open_message_ = true;
}

void ManagerApp::failed(const std::string& what, const std::string& error, bool access_denied) {
    show_message(what, error + (access_denied ? "\n\nWindows refused access to the game folder. The Manager can start "
                                                "again as administrator and retry."
                                              : std::string()));
    offer_elevation_ = access_denied;
}

void ManagerApp::draw_modals() {
    if (open_plan_) ImGui::OpenPopup("###plan");
    if (open_zip_) ImGui::OpenPopup("###zip");
    if (open_approve_) ImGui::OpenPopup("###approve");
    if (open_message_) ImGui::OpenPopup("###message");
    open_plan_ = open_zip_ = open_message_ = open_approve_ = false;
    draw_plan_modal();
    draw_zip_modal();
    draw_approve_modal();

    next_modal(36);
    if (ImGui::BeginPopupModal((message_title_ + "###message").c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        wrapped(message_text_);
        ImGui::Spacing();
        if (offer_elevation_ && ImGui::Button("Restart as administrator")) {
            if (restart_elevated(game_)) close_ = true;
            ImGui::CloseCurrentPopup();
        }
        if (offer_elevation_) ImGui::SameLine();
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
            if (close_after_message_) close_ = true;
        }
        ImGui::EndPopup();
    }
}

void ManagerApp::draw_plan_modal() {
    next_modal(40);
    const bool same = snapshot_->installation->installed() && snapshot_->installation->dll_version == package_->version;
    const std::string title = plan_ && plan_->uninstall ? "Uninstall Anvil"
                              : same                    ? "Repair Anvil " + package_->version
                                                        : "Install Anvil " + package_->version;
    if (!ImGui::BeginPopupModal((title + "###plan").c_str(), nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    if (!plan_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const Plan& plan = plan_->plan;
    if (!plan.refused.empty()) colored_wrapped(kBad, plan.refused);
    for (const std::string& note : plan.notes) bullet(note);
    if (plan.steps.empty() && plan.refused.empty()) wrapped("Nothing to change.");
    if (!plan.steps.empty() && ImGui::TreeNode("changes", "%zu change(s) in %s", plan.steps.size(), u8(game_).c_str())) {
        for (const Step& step : plan.steps) ImGui::BulletText("%s %s", step_text(step.kind), step.path.c_str());
        ImGui::TreePop();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(plan.steps.empty() || !plan.refused.empty() || read_only());
    const Installation& inst = *snapshot_->installation;
    const bool repair = inst.installed() && inst.dll_version == package_->version;
    if (ImGui::Button(plan_->uninstall ? "Uninstall" : repair ? "Repair" : "Install")) {
        ImGui::CloseCurrentPopup();
        run_plan();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        plan_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ManagerApp::draw_zip_modal() {
    next_modal(40);
    if (!ImGui::BeginPopupModal("Install a plugin###zip", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    if (!zip_check_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const PluginZipCheck& check = *zip_check_;
    ImGui::TextUnformatted(zip_name_.c_str());
    ImGui::Separator();
    bool can_install = check.error.empty();
    if (!check.error.empty()) {
        colored_wrapped(kBad, "This zip cannot be installed: " + check.error);
    } else {
        const Manifest& m = *check.report.manifest;
        ImGui::Text("%s %s", m.name.empty() ? m.id.c_str() : m.name.c_str(), check.version.c_str());
        weak("id " + m.id + (m.authors.empty() ? std::string() : ", by " + join(m.authors)));
        if (!m.description.empty()) wrapped(m.description);
        if (!m.repository.empty() && m.repository.starts_with("https://")) {
            if (ImGui::SmallButton("Open its repository")) open_url(m.repository);
        }
        ImGui::Spacing();
        colored_wrapped(kWarn, "A plugin is native code: it runs inside the game with your rights, like any program. "
                               "Install plugins only from authors you trust.");
        if (m.affects_gameplay) {
            bullet("It changes the game. Anvil keeps it out of ironman and multiplayer games unless you allow it there.");
        } else {
            bullet("It says it does not change the game (affects_gameplay = false).");
        }
        if (!check.installed_folder.empty()) {
            bullet("It replaces " + (check.installed_version.empty() ? std::string("the installed copy")
                                                                     : "version " + check.installed_version) +
                   " in anvil/plugins/" + check.installed_folder + ". Its settings and data stay.");
        }
        if (!check.ignored.empty()) weak("Not installed (outside the plugin's folder in the zip): " + join(check.ignored));
        for (const cli::Problem& problem : check.report.problems) {
            colored_wrapped(kWarn, problem.file + (problem.line > 0 ? ":" + std::to_string(problem.line) : std::string()) +
                                       ": " + problem.message);
        }
        if (check.would_skip) {
            colored_wrapped(kBad, "Anvil would not load it with this eu4.exe: " + check.report.skip_reason);
            ImGui::Checkbox("Install it anyway", &zip_anyway_);
            can_install = zip_anyway_;
        } else if (check.report.problems.empty()) {
            colored_wrapped(kGood, "anvil check found no problems with this eu4.exe.");
        }
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(!can_install || read_only());
    if (ImGui::Button("Install")) {
        ImGui::CloseCurrentPopup();
        install_zip();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        zip_.reset();
        zip_check_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace anvil::manager
