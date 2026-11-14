#include "manager_app.hpp"

#include "core/anvil_toml_edit.hpp"
#include "core/crash_history.hpp"
#include "core/game_build.hpp"
#include "core/log.hpp"
#include "core/playset.hpp"
#include "core/semver.hpp"
#include "i18n/cjk_font.hpp"
#include "i18n/translate.hpp"
#include "mods.hpp"
#include "problem_report.hpp"
#include "sidebar.hpp"
#include "start_menu.hpp"
#include "ui.hpp"
#include "ui/plugin_widgets.hpp"
#include "ui/settings_order.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

namespace anvil::manager {
namespace {

namespace fs = std::filesystem;
using namespace ui;
using i18n::N_;
using i18n::tr;
using i18n::trf;
using ::anvil::ui::glyph_item;
using ::anvil::ui::kBackground;
using ::anvil::ui::kLine;
using ::anvil::ui::kOnEmber;
using ::anvil::ui::kText;
using ::anvil::ui::kTextWeak;

// The `language` setting of the overlay in that game folder, when it names one.
std::optional<i18n::Language> overlay_language(const fs::path& game) {
    const SettingsFileResult schema = load_settings(game / "anvil" / "plugins" / "anvil-overlay" / "settings.toml");
    if (!schema.file) return std::nullopt;
    const PlayerSettings player = load_player_settings(*schema.file, game / "anvil" / "data" / "anvil-overlay" / "settings.toml");
    for (const Setting& setting : schema.file->settings) {
        if (setting.name != "language" || setting.type != SettingType::Enum) continue;
        const SettingValue value = player.values[setting.id].value_or(setting.default_value);
        if (value.integer < 0 || static_cast<size_t>(value.integer) >= setting.options.size()) return std::nullopt;
        return i18n::language_of_setting(setting.options[static_cast<size_t>(value.integer)].name);
    }
    return std::nullopt;
}

// The overlay's `release_check` setting in that game folder: true for "on",
// false for "off", nullopt for "ask" (it follows the Manager) or no overlay.
std::optional<bool> overlay_release_answer(const fs::path& game) {
    const SettingsFileResult schema = load_settings(game / "anvil" / "plugins" / "anvil-overlay" / "settings.toml");
    if (!schema.file) return std::nullopt;
    const PlayerSettings player = load_player_settings(*schema.file, game / "anvil" / "data" / "anvil-overlay" / "settings.toml");
    for (const Setting& setting : schema.file->settings) {
        if (setting.name != "release_check" || setting.type != SettingType::Enum) continue;
        const SettingValue value = player.values[setting.id].value_or(setting.default_value);
        if (value.integer < 0 || static_cast<size_t>(value.integer) >= setting.options.size()) return std::nullopt;
        const std::string& name = setting.options[static_cast<size_t>(value.integer)].name;
        if (name == "on") return true;
        if (name == "off") return false;
        return std::nullopt;
    }
    return std::nullopt;
}

// The player's language (#158, lab#93): ANVIL_LANGUAGE, the overlay's
// `language` setting, EU4's settings in the game's user folder, then Steam's
// or Windows' Chinese, Japanese or Korean, with or without Mei: at the first
// start Mei is not installed yet.
void follow_language(const fs::path& game) {
    if (!i18n::forced_language() && !game.empty()) {
        if (const auto language = overlay_language(game)) {
            i18n::set_language(*language);
            return;
        }
    }
    i18n::set_language(i18n::choose_manager_language(eu4_user_dir(game)).language);
}

// The problems of a check to show a player. On a build Anvil does not know,
// every symbol differs and says nothing the first line and the reason do not.
std::span<const cli::Problem> shown_problems(const cli::CheckReport& report) {
    const std::span<const cli::Problem> all(report.problems);
    return report.exe_build.empty() ? all.first(std::min<size_t>(all.size(), 1)) : all;
}

// `text` with its middle cut when it is wider than `width`.
std::string fitted(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    const std::wstring wide = widen(text);
    for (size_t keep = wide.size(); keep > 8; --keep) {
        const std::string cut = narrow(wide.substr(0, keep / 2) + L"..." + wide.substr(wide.size() - (keep - keep / 2)));
        if (ImGui::CalcTextSize(cut.c_str()).x <= width) return cut;
    }
    return text;
}

// The game's version in a known build's id ("eu4-1.37.5-steam-win64" gives "1.37.5"); the id itself otherwise.
std::string build_version(const std::string& build) {
    constexpr std::string_view prefix = "eu4-";
    if (!build.starts_with(prefix)) return build;
    const size_t end = build.find('-', prefix.size());
    return build.substr(prefix.size(), end == std::string::npos ? std::string::npos : end - prefix.size());
}

bool is_standard(const Package& package, const std::string& id) {
    return std::find(package.standard_plugins.begin(), package.standard_plugins.end(), id) != package.standard_plugins.end();
}

// What a module (ADR 0011) does to the game and which opt-ins it offers; the
// the overlay says the same.
std::string module_summary(const Manifest& plugin, const Module& m, const std::string& name) {
    const std::string label = name.empty() ? module_key(plugin, m) : name + " (" + module_key(plugin, m) + ")";
    if (!m.affects_gameplay) return trf("{0}: does not change the game.", {label});
    const bool ironman = m.allow_ironman == Allow::OptIn, multiplayer = m.allow_multiplayer == Allow::OptIn;
    if (ironman && multiplayer) return trf("{0}: changes the game; you can allow it in ironman and multiplayer.", {label});
    if (ironman) return trf("{0}: changes the game; you can allow it in ironman, not in multiplayer.", {label});
    if (multiplayer) return trf("{0}: changes the game; you can allow it in multiplayer, not in ironman.", {label});
    return trf("{0}: changes the game; suspended in ironman and multiplayer.", {label});
}

// A plugin changes the game when it says so or when any of its modules does.
bool changes_game(const Manifest& m) {
    return m.affects_gameplay ||
           std::any_of(m.modules.begin(), m.modules.end(), [](const Module& mod) { return mod.affects_gameplay; });
}

bool listed(const std::vector<std::string>& list, const std::string& id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

const char* step_text(Step::Kind kind) {
    switch (kind) {
    case Step::Kind::Write: return tr("write {0}");
    case Step::Kind::CopyManager: return tr("copy the Manager to {0}");
    case Step::Kind::Remove: return tr("remove {0}");
    case Step::Kind::RemoveTree: return tr("remove the folder {0}");
    case Step::Kind::RemoveIfEmpty: return tr("remove {0} if empty");
    case Step::Kind::RemoveManager: return tr("remove {0}");
    case Step::Kind::Move: return tr("move {0} to {1}");
    }
    return "";
}

constexpr const char* kTroubleshootingUrl = "https://anvil.happydwarf.dev/docs/troubleshooting/#if-anvil-does-not-run-at-all";

std::string megabytes(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(bytes) / (1 << 20));
    return text;
}

// The file name of a pinned zip's URL, and where it comes from
// ("github.com/happy-dwarf-team/<repo>").
std::string zip_file_name(const CatalogPlugin& plugin) { return plugin.zip.url.substr(plugin.zip.url.rfind('/') + 1); }
std::string zip_source(const CatalogPlugin& plugin) {
    constexpr size_t scheme = std::string_view("https://").size();
    return plugin.zip.url.substr(scheme, plugin.zip.url.find("/releases/") - scheme);
}

// The screenshot mode reads no manager.toml and asks nothing: no release question, no check.
ManagerSettings screenshot_settings() {
    ManagerSettings settings;
    settings.asked_releases = true;
    return settings;
}

}  // namespace

ManagerApp::ManagerApp(HWND window, std::optional<Package> package, std::string package_error, fs::path game,
                       std::optional<Screenshot> screenshot)
    : window_(window), package_(std::move(package)), package_error_(std::move(package_error)), exe_(own_exe_path()),
      settings_(screenshot ? screenshot_settings() : load_manager_settings()), benchmark_(window, settings_),
      screenshot_(std::move(screenshot)) {
    const fs::path steam = steam_root_from_registry();
    if (!steam.empty()) steam_games_ = find_steam_games(steam);
    gog_game_ = find_gog_game();
    if (game.empty() && runs_from(exe_, exe_.parent_path()) && inspect_game_folder(exe_.parent_path()).error.empty()) {
        game = exe_.parent_path();  // the copy in <EU4>\AnvilManager.exe
    }
    if (game.empty() && !screenshot_) game = settings_.game_folder;
    if (game.empty() && steam_games_.size() + (gog_game_ ? 1 : 0) == 1) {
        game = gog_game_ ? *gog_game_ : steam_games_.front();
    }
    follow_language(game);
    catalog_ = load_embedded_catalog(catalog_error_);
    if (!game.empty() && package_) choose_game(game);
    if (settings_.check_releases) check_for_updates();
}

void ManagerApp::check_for_updates() {
    if (!release_pending_.valid()) release_pending_ = std::async(std::launch::async, fetch_latest_release);
    if (!catalog_pending_.valid()) {
        catalog_by_check_ = true;
        catalog_pending_ = std::async(std::launch::async, fetch_catalog);
    }
}

ManagerApp::CatalogInputs ManagerApp::catalog_inputs() const {
    CatalogInputs in;
    if (!snapshot_) return in;
    if (const auto& report = snapshot_->report) {
        for (const cli::GamePlugin& plugin : report->plugins) {
            if (plugin.report.manifest) in.local.emplace_back(plugin.report.manifest->id, plugin.report.manifest->version.to_string());
        }
        for (const cli::GameModPlugin& plugin : report->mod_plugins) in.mods.push_back(plugin.id);
    }
    const auto& installation = snapshot_->installation;
    if (installation && installation->installed()) in.anvil_version = installation->dll_version;
    in.build = snapshot_->folder.build;
    return in;
}

std::vector<const CatalogPlugin*> ManagerApp::catalog_updates_here() const {
    if (!catalog_ || !snapshot_ || !snapshot_->report) return {};
    const CatalogInputs in = catalog_inputs();
    return catalog_updates(*catalog_, in.build, in.anvil_version, in.local, in.mods);
}

void ManagerApp::choose_game(const fs::path& dir) {
    game_ = dir.lexically_normal();
    gog_ = is_gog_folder(game_);
    follow_language(game_);
    snapshot_.reset();
    plugins_.reset();
    crash_selected_.clear();
    crash_summaries_.clear();
    crash_text_.clear();
    set_log_text({});
    running_checked_ = {};
    running_started_ = 0;
    running_elsewhere_.clear();
    anvil_missed_ = false;
    launcher_opened_ = false;
    // A player who answered the release question in the game (the overlay's
    // setting) is not asked again here (design §15).
    if (!settings_.asked_releases) {
        if (const auto answer = overlay_release_answer(game_)) {
            settings_.asked_releases = true;
            settings_.check_releases = *answer;
            if (*answer) check_for_updates();
        }
    }
    if (settings_.game_folder != game_ && !screenshot_) {
        settings_.game_folder = game_;
        std::string error;
        save_manager_settings(settings_, error);
    }
    refresh();
}

void ManagerApp::refresh() {
    if (game_.empty() || !package_) return;
    if (pending_.valid()) pending_.wait();
    const std::string build = screenshot_ ? screenshot_->build : std::string();
    pending_ = std::async(std::launch::async, [game = game_, package = &*package_, exe = exe_, build] {
        Snapshot snapshot;
        snapshot.folder = inspect_game_folder(game);
        if (!snapshot.folder.error.empty()) return snapshot;
        if (!build.empty()) snapshot.folder.build = build;  // a fixture's eu4.exe
        snapshot.installation = inspect_installation(*package, game, exe);
        std::error_code ec;
        if (fs::is_directory(game / "anvil", ec)) {
            cli::GameOptions options;
            options.game_dir = game;
            if (!build.empty()) options.build_of = [build](std::string_view) { return build.c_str(); };
            snapshot.report = cli::check_game(options);
        }
        snapshot.anvil_toml = read_anvil_settings(game / "anvil" / "anvil.toml");
        snapshot.last_start = read_last_start(game / "anvil" / "data" / "anvil" / "last-start.toml");
        snapshot.crashes = crash_report_names(game / "anvil" / "crashes");
        return snapshot;
    });
}

void ManagerApp::poll() {
    using namespace std::chrono;
    if (pending_.valid() && pending_.wait_for(0s) == std::future_status::ready) {
        snapshot_ = pending_.get();
        translations_.clear();   // a plugin installed or updated may bring other translations
        follow_language(game_);  // the overlay's setting may be new: the folder was installed or changed
        rebuild_plugins();       // its sources point into the new report
    }
    if (release_pending_.valid() && release_pending_.wait_for(0s) == std::future_status::ready) {
        release_ = release_pending_.get();
    }
    if (catalog_pending_.valid() && catalog_pending_.wait_for(0s) == std::future_status::ready) {
        CatalogFetch fetch = catalog_pending_.get();
        catalog_error_ = fetch.error;
        if (fetch.catalog) {
            catalog_ = std::move(fetch.catalog);
            catalog_fetched_ = true;
        }
    }
    if (download_pending_.valid() && download_pending_.wait_for(0s) == std::future_status::ready) {
        downloaded(download_pending_.get());
    }
    const auto now = steady_clock::now();
    // The screenshot mode draws the folder alone: another game running on this machine would change the
    // picture and show its private path.
    if (!game_.empty() && !screenshot_ && now - running_checked_ > 1500ms) {
        running_checked_ = now;
        const bool was = running_;
        GameProcesses processes = game_processes(game_);
        running_ = processes.here;
        running_elsewhere_ = std::move(processes.elsewhere);
        if (running_) {
            launcher_opened_ = false;  // the note did its job
            anvil_missed_ = false;
            if (processes.here_started != 0) running_started_ = processes.here_started;
            // Without a record of the last start, this start loads what the files hold now.
            if (!was && snapshot_ && !snapshot_->last_start) plugins_take_effect();
        }
        if (was && !running_) {
            // Anvil opens anvil.log first thing once Windows loads its version.dll: a game that ended without
            // writing it ran without Anvil (issue #253). A benchmark series starts the game itself.
            const bool benchmark = benchmark_.busy() || benchmark_busy_;
            anvil_missed_ = !benchmark && anvil_installed() &&
                            anvil_missed_game(file_written(game_ / "anvil" / "anvil.log"), running_started_);
            running_started_ = 0;
        }
        // The game may have written logs and crash reports; a benchmark series
        // is read once it ends, not between its starts.
        if (was && !running_ && !benchmark_.busy()) refresh();
    }
    if (benchmark_busy_ && !benchmark_.busy()) refresh();
    benchmark_busy_ = benchmark_.busy();
}

bool ManagerApp::read_only() const { return running_ || pending_.valid(); }

bool ManagerApp::anvil_installed() const {
    return snapshot_ && snapshot_->installation && snapshot_->installation->dll == Installation::Dll::Anvil;
}

void ManagerApp::draw() {
    poll();
    if (!shot_dialog_.empty() && settled() && snapshot_ && snapshot_->installation && package_) {
        const bool uninstall = shot_dialog_ == "uninstall";
        if (!uninstall || snapshot_->installation->installed()) start_plan(uninstall);
        shot_dialog_.clear();
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    // The window is the whole client area, so it has no border or rounded corners of its own;
    // the sidebar and the page fill it, and the page scrolls on its own.
    const ImVec2 padding = ImGui::GetStyle().WindowPadding;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Anvil Manager", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    // The sidebar only once there is a game to start and pages to show.
    const bool shell = package_ && !game_.empty() && !(snapshot_ && !snapshot_->folder.error.empty());
    if (shell) {
        // Below about 900 px at 100 % the sidebar folds to a rail of icons, so the
        // Plugins list and its detail keep their room (docs/ui-design-system.md, "Shell").
        const float font = ImGui::GetFontSize();
        const bool rail = viewport->WorkSize.x < font * 53.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, std::round(10 * font / 17.0f)));
        ImGui::BeginChild("sidebar", ImVec2(std::round(font * (rail ? 3.3f : 11.8f)), 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        draw_sidebar(rail);
        ImGui::EndChild();
        // Its edge: one line of the separators' colour.
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(max.x - 0.5f, min.y), ImVec2(max.x - 0.5f, max.y), ImGui::GetColorU32(kLine));
        ImGui::SameLine(0, 0);
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(padding.x * 1.5f), std::round(padding.y * 1.25f)));
    ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (!package_) {
        ::anvil::ui::title("Anvil Manager " ANVIL_VERSION);
        colored_wrapped(kBad, trf("This AnvilManager.exe is broken: {0}. Download it again from {1}.", {package_error_, kReleasesUrl}));
    } else if (!shell) {
        ::anvil::ui::title("Anvil Manager " ANVIL_VERSION);
        draw_first_run();
    } else {
        draw_page();
    }
    ImGui::EndChild();
    draw_modals();
    ImGui::End();
}

void ManagerApp::draw_sidebar(bool rail) {
    const float px = ImGui::GetFontSize() / 17.0f;
    const float pad = std::round((rail ? 8 : 12) * px);
    const ImGuiStyle& style = ImGui::GetStyle();

    // Play EU4: the window's main action, on top of everything else.
    const bool no_game = snapshot_ && !snapshot_->folder.error.empty();  // nothing here to start
    ImGui::SetCursorPosX(pad);
    ImGui::BeginDisabled(running_ || no_game || benchmark_.busy());
    const fs::path gog_start = gog_ ? gog_program(game_) : fs::path();
    const std::string play = rail ? std::string("###play") : std::string(tr("Play EU4")) + "###play";
    const float play_width = ImGui::GetContentRegionAvail().x - pad;
    if (primary_button(play.c_str(), ImVec2(play_width, ImGui::GetFrameHeight() + style.FramePadding.y))) {
        const bool started = launch_game(gog_start);
        launcher_opened_ = started && gog_;
        if (!started && gog_) {
            show_message(tr("The Paradox launcher did not start"), trf("Windows could not open {0}.", {u8(gog_start)}));
        } else if (!started) {
            show_message(tr("Steam did not start"), tr("Windows could not open steam://rungameid/236850. Is Steam installed?"));
        }
    }
    if (rail) {
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        draw_play_glyph(ImGui::GetWindowDrawList(), ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f),
                        ImGui::GetTextLineHeight() * 0.62f, ImGui::GetColorU32(kOnEmber));
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        const char* tip = gog_ ? tr("Opens the Paradox launcher, as GOG does. Press Play there.") : tr("Starts the game through Steam");
        if (rail) {
            ImGui::SetTooltip("%s\n%s", tr("Play EU4"), tip);
        } else {
            ImGui::SetTooltip("%s", tip);
        }
    }

    // The game: its store and build, the folder in the tooltip, and Change...
    const std::string folder = u8(game_);
    const std::string build = snapshot_ && snapshot_->folder.error.empty() ? snapshot_->folder.build : std::string();
    const std::string about_folder = build.empty() ? folder : folder + "\neu4.exe: " + build;
    ImGui::BeginDisabled(running_ || benchmark_.busy());
    if (rail) {
        ImGui::SetCursorPosX(pad);
        if (ImGui::Button("...###change", ImVec2(ImGui::GetContentRegionAvail().x - pad, 0))) {
            if (const auto dir = pick_folder(window_, game_)) choose_game(*dir);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s\n%s", tr("Change..."), about_folder.c_str());
        }
    } else {
        ImGui::SetCursorPosX(pad);
        const std::string where = std::string(gog_ ? "GOG" : "Steam") + (build.empty() ? "" : " " + build_version(build));
        weak(where);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", about_folder.c_str());
        if (snapshot_ && snapshot_->folder.error.empty() && build.empty()) {
            ImGui::SameLine();
            glyph_item(anvil::ui::Glyph::Attention);  // a build Anvil does not know: the page says more
        }
        const char* change = tr("Change...");
        const float change_width = ImGui::CalcTextSize(change).x + style.FramePadding.x * 2;
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x - pad < change_width) {
            ImGui::NewLine();
            ImGui::SetCursorPosX(pad);
        }
        if (ImGui::SmallButton(change)) {
            if (const auto dir = pick_folder(window_, game_)) choose_game(*dir);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", about_folder.c_str());
    }
    ImGui::Dummy(ImVec2(0, std::round(8 * px)));

    // The pages: what a player uses, then the tools, and About at the bottom.
    // While a benchmark series runs, every page but Benchmark is locked.
    const bool locked = benchmark_.busy();
    if (locked) page_ = "Benchmark";
    const size_t updates = snapshot_ ? catalog_updates_here().size() : 0;
    const size_t waiting = plugins_ ? anvil::ui::to_approve(plugins_->model) : 0;
    const auto entry = [&](NavEntry e) {
        ImGui::BeginDisabled(locked && std::string_view(e.id) != "Benchmark");
        if (nav_item(e, page_ == e.id, rail)) page_ = e.id;
        ImGui::EndDisabled();
    };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, std::round(2 * px)));
    entry({N_("Anvil"), NavIcon::Anvil});
    // What waits for the player rides on the entry (ui-guidelines §5): plugins
    // to approve, and updates in the catalog (ADR 0006).
    entry({N_("Plugins"), NavIcon::Plugins, waiting, true,
           waiting > 0 ? trf("Plugins ({0} to approve)", {std::to_string(waiting)}) : std::string()});
    entry({N_("Catalog"), NavIcon::Catalog, updates, false,
           updates > 0 && rail ? std::string(tr("Catalog")) + " (" + std::to_string(updates) + ")" : std::string()});
    if (rail) {
        ImGui::Dummy(ImVec2(0, std::round(4 * px)));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x + pad, at.y), ImVec2(at.x + w - pad, at.y), ImGui::GetColorU32(kLine));
        ImGui::Dummy(ImVec2(0, std::round(5 * px)));
    } else {
        ImGui::Dummy(ImVec2(0, std::round(8 * px)));
        ImGui::SetCursorPosX(pad);
        weak(tr("Tools"));
    }
    entry({N_("Benchmark"), NavIcon::Benchmark});
    entry({N_("Crashes"), NavIcon::Crashes});
    entry({N_("Log"), NavIcon::Log});
    // About sits at the bottom when the window is tall enough, else right after the tools.
    const float bottom = ImGui::GetWindowHeight() - style.WindowPadding.y - nav_item_height();
    if (ImGui::GetCursorPosY() < bottom) ImGui::SetCursorPosY(bottom);
    entry({N_("About"), NavIcon::About});
    ImGui::PopStyleVar();
}

void ManagerApp::draw_page() {
    draw_notices();
    ::anvil::ui::title(tr(page_.c_str()));
    if (!snapshot_) {
        weak(tr("Reading the game folder..."));
        return;
    }
    if (page_ == "Catalog") {
        draw_catalog_tab();
    } else if (page_ == "Plugins") {
        draw_plugins_tab();
    } else if (page_ == "Benchmark") {
        draw_benchmark_tab();
    } else if (page_ == "Crashes") {
        draw_crashes_tab();
    } else if (page_ == "Log") {
        draw_log_tab();
    } else if (page_ == "About") {
        draw_about_tab();
    } else {
        page_ = "Anvil";
        draw_anvil_tab();
    }
}

// What concerns every page: the game's build, a game that runs, a series that runs.
void ManagerApp::draw_notices() {
    const float before = ImGui::GetCursorPosY();
    if (snapshot_ && snapshot_->folder.error.empty() && snapshot_->folder.build.empty()) {
        colored_wrapped(kWarn, tr("eu4.exe is not a build Anvil knows (EU4 updated?): plugins that need game "
                                  "addresses are skipped, the others load."));
        if (snapshot_->folder.gog) {
            colored_wrapped(kWarn, i18n::text(i18n::Message("build_unknown_gog", {known_gog_version()})));
            if (!snapshot_->folder.gog_version.empty()) weak(trf("GOG calls this version {0}.", {snapshot_->folder.gog_version}));
        }
    }
    if (launcher_opened_ && !running_) {
        weak_wrapped(tr("The Paradox launcher opens: press Play there and Anvil loads with the game. On its first start it shows Paradox's EULA before Play. Only you can accept it; it comes from the launcher, not from Anvil."));
    }
    if (benchmark_.busy()) {
        colored_wrapped(kWarn, tr("A start-up benchmark series is running: EU4 starts and quits by itself. The other "
                                  "pages are locked until the series ends."));
    } else if (running_) {
        colored_wrapped(kWarn, tr("EU4 is running from this folder. Close the game to change Anvil, plugins or "
                                  "settings; until then the Manager only shows them."));
    } else if (!running_elsewhere_.empty() && anvil_installed()) {
        // Steam starts its own copy whatever folder the Manager shows (issue #253).
        const fs::path& other = running_elsewhere_.front();
        colored_wrapped(kWarn, trf("EU4 is running from {0}, not from the folder Anvil is installed in.", {u8(other)}));
        if (ImGui::SmallButton(tr("Switch to that folder"))) {
            const fs::path chosen = other;  // choose_game clears running_elsewhere_
            choose_game(chosen);
        }
    }
    if (anvil_missed_ && !running_ && !benchmark_.busy()) {
        colored_wrapped(kWarn, tr("The game ran, but Anvil did not load."));
        if (ImGui::SmallButton(tr("What to check"))) open_url(kTroubleshootingUrl);
    }
    if (ImGui::GetCursorPosY() > before) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }
}

void ManagerApp::draw_first_run() {
    if (snapshot_ && !snapshot_->folder.error.empty()) {
        banner("folder-error", Tone::Bad, u8(game_) + ": " + snapshot_->folder.error + ".");
        ImGui::Spacing();
    }
    wrapped(tr("Choose the Europa Universalis IV folder: the one with eu4.exe."));
    ImGui::Spacing();
    // One card per copy Steam or GOG lists; the first one's Use is the view's main action.
    bool first = true;
    const auto use = [&](const char* store, const fs::path& game) {
        ImGui::PushID(u8(game).c_str());
        begin_card("game");
        const char* label = tr("Use");
        ImGui::TextUnformatted(store);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ::anvil::ui::button_width(label)));
        if (first ? primary_button(label) : ImGui::Button(label)) choose_game(game);
        weak_wrapped(u8(game));
        end_card();
        first = false;
        ImGui::PopID();
    };
    if (!steam_games_.empty()) {
        weak(tr("Steam has the game here:"));
        for (const fs::path& game : steam_games_) use("Steam", game);
        ImGui::Spacing();
    }
    if (gog_game_) {
        weak(tr("GOG has the game here:"));
        use("GOG", *gog_game_);
        ImGui::Spacing();
    }
    if (steam_games_.empty() && !gog_game_) state_row(anvil::ui::Glyph::Off, tr("Neither Steam nor GOG lists the game."), kTextWeak);
    if (ImGui::Button(tr("Choose the folder..."))) {
        if (const auto dir = pick_folder(window_, game_)) choose_game(*dir);
    }
}

void ManagerApp::draw_anvil_tab() {
    const Installation& inst = *snapshot_->installation;
    const std::string& version = package_->version;
    const auto installed_version = Version::parse(inst.dll_version);
    const auto own_version = Version::parse(version);
    const bool newer_installed = installed_version && own_version && *own_version < *installed_version;

    // What is installed, in one banner with what to do about it (docs/ui-design-system.md, "Banner").
    Tone tone = Tone::Info;
    std::string state;
    switch (inst.dll) {
    case Installation::Dll::Missing:
        state = inst.anvil_folder ? tr("Anvil is not installed (no version.dll). The anvil folder still has your "
                                       "plugins and settings: installing brings them back.")
                                  : tr("Anvil is not installed. The game starts as usual (vanilla).");
        break;
    case Installation::Dll::Foreign:
        tone = Tone::Warn;
        if (inst.legacy_over_anvil()) {
            // Its instructions may say to install it again (lab#104).
            state = trf("{0} replaced Anvil's version.dll. Your plugins and settings are still in the anvil "
                        "folder: replace {0} again to bring Anvil back.",
                        {inst.legacy_name()});
        } else if (!inst.legacy.empty()) {
            state = trf("{0} is installed in this folder. Anvil replaces it: its files move to {1}, and "
                        "uninstalling Anvil can put them back.",
                        {inst.legacy_name(), kLegacyBackup});
        } else {
            state = tr("version.dll in the game folder is not Anvil's. Another mod or tool may use it: "
                       "Anvil replaces it only if you say so.");
        }
        break;
    case Installation::Dll::Anvil:
        if (inst.dll_version != version) {
            tone = newer_installed ? Tone::Warn : Tone::Info;
            state = trf("Anvil {0} is installed. This Manager carries Anvil {1}.", {inst.dll_version, version});
        } else if (inst.intact()) {
            tone = Tone::Good;
            state = trf("Anvil {0} is installed and complete.", {version});
        } else {
            tone = Tone::Warn;
            state = trf("Anvil {0} is installed, but some of its files differ from its package:", {version});
        }
        break;
    }
    ImGui::Spacing();
    begin_banner("state", tone);
    wrapped(state);
    if (inst.dll == Installation::Dll::Anvil && inst.dll_version != version && newer_installed) {
        wrapped(trf("The installed Anvil is newer: installing this one would go back to {0}. "
                    "Use the Manager of the newer release instead.",
                    {version}));
    }
    if (inst.manager_copy && !inst.manager_current && !runs_from(exe_, game_)) {
        weak_wrapped(tr("AnvilManager.exe in the game folder is another version than this one: installing replaces it."));
    }
    if (inst.dll == Installation::Dll::Anvil && inst.dll_version == version && !inst.intact()) {
        // The files, folded: a damaged install can list dozens.
        std::vector<std::string> files;
        for (const auto& path : inst.missing) {
            if (path != kPlayerToml) files.push_back(trf("{0} is missing", {path}));
        }
        for (const auto& path : inst.differing) files.push_back(trf("{0} differs", {path}));
        for (const auto& path : inst.extra) files.push_back(trf("{0} does not belong to Anvil", {path}));
        if (ImGui::TreeNode("files", "%s", trf("Show the files ({0})", {std::to_string(files.size())}).c_str())) {
            for (const std::string& file : files) bullet(file);
            ImGui::TreePop();
        }
    }
    ImGui::BeginDisabled(read_only());
    const std::string label = !inst.legacy.empty()                 ? trf("Replace {0}", {inst.legacy_name()})
                              : inst.dll != Installation::Dll::Anvil ? trf("Install Anvil {0}", {version})
                              : inst.dll_version != version         ? trf("Install this Anvil {0}", {version})
                                                                    : std::string(tr("Repair"));
    const bool unknown_dll = inst.dll == Installation::Dll::Foreign && inst.legacy.empty();
    if (unknown_dll) ImGui::Checkbox(tr("Replace the other version.dll"), &replace_foreign_dll_);
    const bool nothing = inst.intact() && inst.dll_version == version && inst.manager_current;
    ImGui::BeginDisabled(nothing || (unknown_dll && !replace_foreign_dll_));
    // Installing or repairing is the view's main action while there is something to do.
    if (nothing ? ImGui::Button((label + "###install").c_str()) : primary_button((label + "###install").c_str())) start_plan(false);
    ImGui::EndDisabled();
    if (nothing) {
        ImGui::SameLine();
        weak(tr("nothing to repair"));
    }
    ImGui::EndDisabled();
    weak_wrapped(trf("Copies version.dll, the standard plugins ({0}) and this Manager into the game folder. Your anvil.toml, "
                     "plugins and their settings stay.",
                     {join(package_->standard_plugins)}));
    end_banner(tone);

    if (!settings_.asked_releases) {
        ImGui::Spacing();
        begin_banner("ask-releases", Tone::Info);
        wrapped(tr("Should Anvil ask GitHub for new releases when the Manager or the game starts? The game asks about "
                   "Anvil; the Manager also reads the list of the team's plugins and shows which installed ones have an "
                   "update. Nothing is downloaded or installed until you click. You can change this in About, and for "
                   "the game in the Anvil panel's settings."));
        const auto answer = [&](bool yes) {
            settings_.asked_releases = true;
            settings_.check_releases = yes;
            std::string error;
            save_manager_settings(settings_, error);
            if (yes) check_for_updates();
        };
        if (ImGui::Button(tr("Yes, check for releases"))) answer(true);
        ImGui::SameLine();
        if (ImGui::Button(tr("No"))) answer(false);
        end_banner(Tone::Info);
    }
    if (release_ && release_->release && newer_release(release_->release->tag, ANVIL_VERSION)) {
        ImGui::Spacing();
        begin_banner("release", Tone::Info);
        ImGui::TextUnformatted(trf("Anvil {0} is available.", {release_->release->tag}).c_str());
        ImGui::SameLine();
        if (text_button(tr("Open its page"))) open_url(release_->release->url);
        end_banner(Tone::Info);
    }

    // The facts of this folder.
    ImGui::Spacing();
    ImGui::SeparatorText(tr("Installed"));
    if (begin_facts("installed")) {
        fact("Anvil", inst.dll == Installation::Dll::Anvil ? inst.dll_version : std::string("-"));
        fact("eu4.exe", snapshot_->folder.build.empty() ? std::string("-") : snapshot_->folder.build);
        fact(tr("Folder"), u8(game_));
        end_facts();
    }
    if (inst.manager_copy) {
        if (start_menu_game_ != game_) {  // read once per game, and again after a change here
            start_menu_game_ = game_;
            in_start_menu_ = link_starts(start_menu_link(), game_ / kManagerExe);
        }
        if (in_start_menu_) {
            state_row(anvil::ui::Glyph::On, tr("Anvil Manager is in the Start menu."));
        } else if (text_button(tr("Add to the Start menu"))) {
            std::string error;
            if (write_link(start_menu_link(), game_ / kManagerExe, error)) {
                in_start_menu_ = true;
            } else {
                failed(tr("The Start menu entry was not added"), error, false);
            }
        }
    }

    // Uninstalling: apart from the rest, and asked in its dialog (with what to remove too).
    ImGui::Spacing();
    ImGui::SeparatorText(tr("Uninstall"));
    weak_wrapped(tr("Without version.dll the game starts vanilla at once."));
    ImGui::BeginDisabled(read_only() || (inst.dll != Installation::Dll::Anvil && !inst.anvil_folder && !inst.manager_copy));
    if (ImGui::Button(tr("Uninstall Anvil..."))) start_plan(true);
    ImGui::EndDisabled();
}

const std::string& ManagerApp::plugin_text(const fs::path& folder, const std::string& english) const {
    const std::string code = i18n::language_code(i18n::language());
    if (english.empty() || folder.empty() || code == "en") return english;
    auto it = translations_.find({folder, code});
    if (it == translations_.end()) {
        it = translations_.emplace(std::pair{folder, code}, load_plugin_translation(folder, code).translation).first;
    }
    if (!it->second) return english;
    const auto found = it->second->text.find(english);
    return found != it->second->text.end() ? found->second : english;
}

void ManagerApp::draw_catalog_tab() {
    const auto& report = snapshot_->report;
    wrapped(gog_ ? tr("Plugins of the Anvil team. Install puts one in anvil/plugins.")
                 : tr("Plugins of the Anvil team. Install puts one in anvil/plugins; a plugin from the Steam Workshop comes "
                      "with its mod instead."));
    // Where the list comes from, and the link that asks GitHub again.
    ImGui::BeginDisabled(catalog_pending_.valid());
    if (text_button(tr("Get the current list"))) {
        catalog_by_check_ = false;
        catalog_pending_ = std::async(std::launch::async, fetch_catalog);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (catalog_pending_.valid()) weak(tr("asking GitHub..."));
    else if (!catalog_fetched_) weak(trf("The list of Manager {0}.", {ANVIL_VERSION}));
    else weak(catalog_by_check_ ? tr("The list from GitHub, read with the release check.") : tr("The list from GitHub."));
    if (settings_.check_releases) {
        weak_wrapped(tr("You agreed that Anvil asks GitHub for new releases, so the Manager reads this list from the anvil "
                        "repository when it starts (About turns it off). A plugin's zip is downloaded from its GitHub "
                        "release only when you click, and installed only if its SHA-256 equals the list's."));
    } else {
        weak_wrapped(tr("Nothing is downloaded until you click: the list from the anvil repository on GitHub, and a plugin's "
                        "zip from its GitHub release, installed only if its SHA-256 equals the list's."));
    }
    if (!catalog_error_.empty()) banner("refused", Tone::Bad, trf("The list was refused: {0}", {catalog_error_}));
    if (!catalog_) return;
    if (!report) banner("no-anvil", Tone::Info, tr("Install Anvil first, then its plugins."));
    if (download_pending_.valid() && download_) weak(trf("Downloading {0}...", {zip_file_name(*download_)}));
    ImGui::Spacing();

    const CatalogInputs in = catalog_inputs();
    const std::string& build = in.build;
    const bool steam = !gog_;
    const float line = ImGui::GetTextLineHeight();
    const float indent = line + ImGui::GetStyle().ItemInnerSpacing.x * 2;

    // A card per plugin, as the Plugins list draws an entry: its state's glyph,
    // the name in semibold, then what it does, its state and its actions.
    for (const CatalogPlugin& plugin : catalog_->plugins) {
        const CatalogStatus status = catalog_status(plugin, build, in.anvil_version, in.local, in.mods);
        const auto installed = Version::parse(status.installed);
        const auto pinned = Version::parse(plugin.version);
        const bool newer_installed = installed && pinned && *pinned < *installed;
        const bool problem = !status.for_build || !status.anvil_ok;
        ImGui::PushID(plugin.id.c_str());
        begin_card("plugin");
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (problem) {
            anvil::ui::draw_glyph(draw, at, line, anvil::ui::Glyph::Attention);
        } else if (status.current(plugin)) {
            anvil::ui::draw_glyph(draw, at, line, anvil::ui::Glyph::On);
        } else if (status.update(plugin)) {
            anvil::ui::draw_pending_dot(draw, ImVec2(at.x + line * 0.5f, at.y + line * 0.5f), line * 0.22f);
        }
        ImGui::Indent(indent);
        anvil::ui::strong(plugin.name.c_str());
        ImGui::SameLine();
        weak(plugin.version);
        if (plugin.affects_gameplay) {
            ImGui::SameLine();
            tag(tr("changes the game"), true);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tr("It changes the game. Anvil keeps it out of ironman and multiplayer games unless you allow it there."));
            }
        }
        const char* code = i18n::language_code(i18n::language());
        if (!plugin.summary.empty()) wrapped(plugin.summary_in(code));
        if (!plugin.note.empty()) weak_wrapped(plugin.note_in(code));

        // Its state: an exception in its colour, the usual in weak text.
        bool can_install = report && report->cannot_check.empty();
        if (!status.for_build) {
            colored_wrapped(kWarn, build.empty() ? std::string(tr("It runs only on a game build Anvil knows."))
                                                 : trf("Not made for this game's build; it runs on {0}.", {join(plugin.builds)}));
            can_install = false;
        } else if (!status.anvil_ok) {
            colored_wrapped(kWarn, trf("It needs Anvil {0}: run the Manager of a newer Anvil first.", {plugin.anvil}));
            can_install = false;
        }
        if (status.current(plugin)) {
            weak_wrapped(tr("Installed."));
        } else if (status.update(plugin)) {
            colored_wrapped(kInfo, trf("Version {0} is installed; {1} is available.", {status.installed, plugin.version}));
        } else if (!status.installed.empty()) {
            weak_wrapped(trf("Version {0} is installed.", {status.installed}));
        }
        if (status.from_mod) {
            weak_wrapped(status.installed.empty()
                             ? tr("An enabled mod carries it (see From mods on the Plugins page). A copy installed here would load instead.")
                             : tr("An enabled mod carries it too; the copy in anvil/plugins loads."));
        }

        // Its actions: Install or Update as a button, the links as text buttons.
        if (!status.current(plugin) && !newer_installed) {
            ImGui::BeginDisabled(!can_install || read_only() || download_pending_.valid());
            if (ImGui::Button(status.installed.empty() ? tr("Install") : tr("Update"))) {
                download_ = plugin;
                open_download_ = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
        }
        if (text_button(tr("Open its page"))) open_url(plugin.page);
        if (!plugin.workshop.empty() && steam) {
            ImGui::SameLine();
            if (text_button(tr("Subscribe on Steam Workshop"))) open_url(plugin.workshop);
        }
        ImGui::Unindent(indent);
        end_card();
        ImGui::PopID();
    }
}

void ManagerApp::draw_approve_modal() {
    next_modal(40);
    if (!begin_dialog("###approve", tr("Approve a plugin from a mod"))) return;
    if (!approve_ || !snapshot_ || !snapshot_->report) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const cli::GameModPlugin& p = *approve_;
    const cli::CheckReport& offered = p.offered();  // of an update, the new files and not the copy that loads
    const Manifest& m = *offered.manifest;
    ImGui::Text("%s %s", m.name.empty() ? m.id.c_str() : m.name.c_str(), p.version.c_str());
    weak(m.authors.empty() ? trf("id {0}", {m.id}) : trf("id {0}, by {1}", {m.id, join(m.authors)}));
    weak(trf("from {0}", {mod_source(p)}));
    if (!m.description.empty()) wrapped(plugin_text(m.folder, m.description));
    if (!m.repository.empty() && m.repository.starts_with("https://")) {
        if (ImGui::SmallButton(tr("Open its repository"))) open_url(m.repository);
    }
    ImGui::Spacing();
    banner("native", Tone::Warn, tr("A plugin is native code: it runs inside the game with your rights, like any program. "
                                    "Approve plugins only from authors you trust."));
    if (changes_game(m)) {
        bullet(tr("It changes the game. Anvil keeps it out of ironman and multiplayer games unless you allow it there."));
    } else {
        bullet(tr("It says it does not change the game (affects_gameplay = false)."));
    }
    if (!m.modules.empty()) {
        bullet(trf("Modules ({0}); one approval covers all of them:", {std::to_string(m.modules.size())}));
        ImGui::Indent();
        for (const Module& mod : m.modules) {
            const std::string text = module_summary(m, mod, plugin_text(m.folder, mod.name));
            bullet(mod.enabled_by_default ? text : text + " " + tr("Off until you turn it on."));
        }
        ImGui::Unindent();
    }
    bullet(offered.has_symbols ? trf("{0} game address(es) declared in symbols.toml", {std::to_string(offered.symbols)})
                               : std::string(tr("It declares no game addresses (no symbols.toml).")));
    bullet(trf("Hash of its files: {0}", {p.hash}));
    if (catalog_) {
        if (const CatalogPlugin* claimed = impersonated_plugin(*catalog_, m.id, m.name, m.repository, p.workshop_id)) {
            banner("impersonated", Tone::Bad,
                   trf("It presents itself as {0}, but it is not the Workshop item the catalog lists for {0}. "
                       "Its name, authors and repository are what the mod says: Anvil cannot check them.",
                       {claimed->name}));
        }
    }
    if (p.state == "update-pending") bullet(trf("It replaces the approved {0} from the next start.", {p.approved_version}));
    if (p.state == "conflict") bullet(tr("Other enabled mods have a plugin with this id; this approval picks this one."));
    if (!offered.skip_reason.empty()) {
        banner("skip", Tone::Bad, trf("Anvil would still skip it: {0}", {i18n::text(offered.skip_why)}));
    }
    dialog_buttons({tr("Approve"), tr("Cancel")});
    if (primary_button(tr("Approve"))) {
        std::string error;
        if (!approve_shown_plugin(game_, eu4_user_dir(game_), *snapshot_->report, p, error)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            approve_.reset();
            failed(tr("The plugin was not approved"), error, false);
            refresh();
            return;
        }
        approve_.reset();
        refresh();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) {
        approve_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ManagerApp::draw_benchmark_tab() {
    BenchmarkContext context;
    context.game = game_;
    context.gog = gog_;
    if (snapshot_->report && snapshot_->report->cannot_check.empty()) context.report = &*snapshot_->report;
    if (snapshot_->installation && snapshot_->installation->dll == Installation::Dll::Anvil) {
        context.installed_anvil = snapshot_->installation->dll_version;
    }
    context.exe_build = snapshot_->folder.build;
    context.exe_sha256 = snapshot_->folder.exe_sha256;
    benchmark_.draw(context);
}

void ManagerApp::draw_crashes_tab() {
    const fs::path folder = game_ / "anvil" / "crashes";
    if (text_button(tr("Open the crashes folder"))) open_folder(folder);
    ImGui::Spacing();
    if (snapshot_->crashes.empty()) {
        state_row(anvil::ui::Glyph::On, tr("No crash reports. When the game crashes with Anvil, a report is written here (the last 10 are kept)."));
        return;
    }
    const auto summary = [&](const std::string& name) -> const std::optional<CrashSummary>& {
        auto it = crash_summaries_.find(name);
        if (it == crash_summaries_.end()) it = crash_summaries_.emplace(name, load_crash_summary(folder, name)).first;
        return it->second;
    };
    const auto what = [](const CrashSummary& crash) {
        const bool named = !crash.summary.empty() || !crash.plugins.empty();
        return named ? i18n::text(i18n::crash_message(crash.level, crash.plugins, crash.summary)) : std::string(tr("a crash"));
    };
    if (crash_selected_.empty()) {
        crash_selected_ = snapshot_->crashes.front();
        crash_text_ = read_text_file(folder / widen(crash_selected_ + ".txt"), 1 << 20).value_or(tr("(the report's text cannot be read)"));
    }

    // The list, as the Plugins list draws its entries: a glyph, the time, and what crashed under it.
    const float line = ImGui::GetTextLineHeight();
    const float list_width = std::clamp(ImGui::GetContentRegionAvail().x * 0.34f, ImGui::GetFontSize() * 14, ImGui::GetFontSize() * 22);
    ImGui::BeginChild("list", ImVec2(list_width, 0), ImGuiChildFlags_Borders);
    for (const std::string& name : snapshot_->crashes) {
        const std::optional<CrashSummary>& crash = summary(name);
        const float pad = ImGui::GetStyle().FramePadding.y;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float right = at.x + ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable(("##" + name).c_str(), name == crash_selected_, ImGuiSelectableFlags_None, ImVec2(0, line * 2 + pad * 2))) {
            crash_selected_ = name;
            crash_text_ = read_text_file(folder / widen(name + ".txt"), 1 << 20).value_or(tr("(the report's text cannot be read)"));
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float x = at.x + line + ImGui::GetStyle().ItemInnerSpacing.x * 2;
        anvil::ui::draw_glyph(draw, ImVec2(at.x + 2, at.y + pad), line, anvil::ui::Glyph::CannotLoad);
        draw->AddText(ImVec2(x, at.y + pad), ImGui::GetColorU32(kText), crash && !crash->time.empty() ? crash->time.c_str() : name.c_str());
        const std::string sub = fitted(crash ? what(*crash) : std::string(tr("The report cannot be read.")), right - x);
        draw->AddText(ImVec2(x, at.y + pad + line), ImGui::GetColorU32(kTextWeak), sub.c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // The detail: what crashed in a banner with its actions, then the report.
    ImGui::BeginChild("report", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (const std::optional<CrashSummary>& crash = summary(crash_selected_)) {
        ::anvil::ui::title(crash->time.empty() ? crash_selected_.c_str() : crash->time.c_str());
        begin_banner("crash", Tone::Bad);
        wrapped(what(*crash));
        if (!crash->plugins.empty()) wrapped(trf("Plugins involved: {0}", {join(crash->plugins)}));
        if (crash->before_game_ready) weak(tr("Before the game was ready."));
        if (!crash->issues.empty()) {
            if (primary_button(tr("Report it to the plugin's author"))) open_url(crash->issues);
            ImGui::SameLine();
        }
        if (text_button(tr("Copy the report"))) copy_to_clipboard(window_, crash_text_);
        end_banner(Tone::Bad);
        ImGui::Spacing();
        begin_mono();
        ImGui::InputTextMultiline("##text", &crash_text_, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
        end_mono();
    } else {
        banner("unreadable", Tone::Bad, tr("The report cannot be read."));
    }
    ImGui::EndChild();
}

void ManagerApp::set_log_text(std::string text) {
    log_text_ = std::move(text);
    log_lines_.clear();
    std::string_view rest = log_text_;
    while (!rest.empty()) {
        const size_t end = rest.find('\n');
        std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto level = log_line_level(line);
        log_lines_.push_back({line, level ? static_cast<int>(*level) : -1});
    }
}

void ManagerApp::draw_log_tab() {
    bool& previous = log_previous_;
    bool reload = log_text_.empty();
    // Which log, then what to do with it.
    const char* sources[] = {tr("Last start"), tr("The start before")};
    const bool enabled[] = {true, true};
    const int shown = log_report_ ? -1 : previous ? 1 : 0;
    if (const int chosen = anvil::ui::segmented("source", sources, enabled, nullptr, 2, shown, false); chosen >= 0) {
        previous = chosen == 1;
        reload = true;
    }
    ImGui::SameLine();
    if (text_button(tr("Reload"))) reload = true;
    ImGui::SameLine();
    if (text_button(tr("Copy"))) copy_to_clipboard(window_, log_text_);
    ImGui::SameLine();
    if (text_button(tr("Open the folder"))) open_folder(game_ / "anvil");
    if (reload) {
        log_report_ = false;
        const fs::path file = game_ / "anvil" / (previous ? "anvil.prev.log" : "anvil.log");
        std::string text = read_text_tail(file, 2000, 512 << 10);
        if (text.empty()) text = trf("({0} is empty or missing: Anvil writes it when the game starts)", {u8(file.filename())});
        set_log_text(std::move(text));
    }

    // The problem report (issue #186): what the bug forms ask for, with private paths shortened, shown where the
    // log was so the player sees what the clipboard holds.
    ImGui::Spacing();
    begin_card("report");
    strong(tr("Report a problem"));
    if (log_report_) {
        weak_wrapped(report_copied_ ? tr("The report is in the clipboard. Nothing is uploaded.")
                                    : tr("The clipboard could not be opened."));
        weak_wrapped(tr("Check it before you post it. If a plugin is skipped or named in a crash, report it to that "
                        "plugin's author instead."));
    } else {
        weak_wrapped(tr("For an issue: versions, the game build, the plugins and why one is skipped, and the end of "
                        "anvil.log, with your folders and Windows user name left out."));
    }
    if (ImGui::Button(tr("Copy a problem report"))) {
        set_log_text(make_problem_report());
        log_report_ = true;
        report_copied_ = copy_to_clipboard(window_, log_text_);
    }
    ImGui::SameLine();
    if (text_button(tr("Open an Anvil issue"))) open_url("https://github.com/happy-dwarf-team/anvil/issues/new/choose");
    end_card();

    // The levels a log line can be filtered to, with their counts; the report has none.
    size_t warnings = 0, errors = 0;
    for (const LogLine& l : log_lines_) {
        warnings += l.level == ANVIL_LOG_WARNING ? 1 : 0;
        errors += l.level == ANVIL_LOG_ERROR ? 1 : 0;
    }
    if (warnings + errors > 0 || log_warnings_ || log_errors_) {
        ImGui::Spacing();
        if (chip((trf("Warnings ({0})", {std::to_string(warnings)}) + "##warnings").c_str(), log_warnings_)) log_warnings_ = !log_warnings_;
        ImGui::SameLine();
        if (chip((trf("Errors ({0})", {std::to_string(errors)}) + "##errors").c_str(), log_errors_)) log_errors_ = !log_errors_;
    }
    const bool filtering = log_warnings_ || log_errors_;
    std::vector<size_t> rows;
    rows.reserve(log_lines_.size());
    for (size_t i = 0; i < log_lines_.size(); ++i) {
        const int level = log_lines_[i].level;
        if (!filtering || (log_warnings_ && level == ANVIL_LOG_WARNING) || (log_errors_ && level == ANVIL_LOG_ERROR)) rows.push_back(i);
    }

    // The lines in the monospace font, a warning's and an error's glyph in a gutter.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ::anvil::ui::kPanel);
    ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleColor();
    begin_mono();
    const float line = ImGui::GetTextLineHeight();
    const float gutter = line + ImGui::GetStyle().ItemInnerSpacing.x * 2;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const LogLine& l = log_lines_[rows[static_cast<size_t>(i)]];
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (l.level == ANVIL_LOG_WARNING || l.level == ANVIL_LOG_ERROR) {
                anvil::ui::draw_glyph(ImGui::GetWindowDrawList(), at, line,
                                      l.level == ANVIL_LOG_ERROR ? anvil::ui::Glyph::CannotLoad : anvil::ui::Glyph::Attention);
            }
            ImGui::SetCursorScreenPos(ImVec2(at.x + gutter, at.y));
            ImGui::TextUnformatted(l.text.data(), l.text.data() + l.text.size());
        }
    }
    end_mono();
    ImGui::EndChild();
}

std::string ManagerApp::make_problem_report() const {
    ProblemReportInput input;
    input.manager_version = ANVIL_VERSION;
    input.installation = snapshot_->installation;
    input.exe_sha256 = snapshot_->folder.exe_sha256;
    input.build = snapshot_->folder.build;
    input.gog = snapshot_->folder.gog;
    input.gog_version = snapshot_->folder.gog_version;
    if (snapshot_->report) input.game = &*snapshot_->report;
    if (!snapshot_->crashes.empty()) input.newest_crash = load_crash_summary(game_ / "anvil" / "crashes", snapshot_->crashes.front());
    input.log_name = "anvil.log";
    input.log_tail = read_text_tail(game_ / "anvil" / "anvil.log", kProblemReportLogLines, kProblemReportLogBytes);
    input.steam_folders = steam_games_.size();
    input.gog_folder = gog_game_.has_value();
    for (size_t i = 0; i < steam_games_.size(); ++i) {
        if (same_file(steam_games_[i], game_)) input.chosen_steam = i;
    }
    input.chosen_gog = gog_game_ && same_file(*gog_game_, game_);
    input.running_elsewhere = running_elsewhere_.size();
    input.anvil_missed = anvil_missed_;

    const auto environment = [](const wchar_t* name) {
        wchar_t value[512];
        const DWORD n = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
        return n > 0 && n < std::size(value) ? std::wstring(value, n) : std::wstring();
    };
    input.folders.push_back({game_, "<game>"});
    if (snapshot_->report && !snapshot_->report->user_dir.empty()) {
        input.folders.push_back({fs::path(widen(snapshot_->report->user_dir)), "<eu4 user folder>"});
    }
    if (const std::wstring profile = environment(L"USERPROFILE"); !profile.empty()) input.folders.push_back({profile, "<user folder>"});
    input.user_name = u8(environment(L"USERNAME"));
    return problem_report(input);
}

void ManagerApp::draw_about_tab() {
    wrapped(trf("Anvil Manager {0} installs Anvil {1}, the native plugin loader for Europa Universalis IV 1.37.5, and "
                "manages its plugins.",
                {ANVIL_VERSION, package_->version}));
    weak_wrapped(tr("Unofficial community project, not affiliated with Paradox Interactive. MIT License."));
    if (text_button(tr("Anvil on GitHub"))) open_url("https://github.com/happy-dwarf-team/anvil");
    ImGui::Spacing();
    ImGui::SeparatorText(tr("New releases"));
    bool check = settings_.check_releases;
    if (ImGui::Checkbox(tr("Ask GitHub for new releases of Anvil and its plugins when the Manager or the game starts"), &check)) {
        settings_.check_releases = check;
        settings_.asked_releases = true;
        std::string error;
        save_manager_settings(settings_, error);
        if (check) check_for_updates();
    }
    weak_wrapped(tr("For Anvil a link is shown, and the Catalog and Plugins pages show plugin updates; nothing is downloaded "
                    "until you click. To update Anvil, run the Manager of the new release."));
    weak_wrapped(tr("The game follows this answer unless the Anvil panel's setting \"Check for new Anvil releases\" says On or Off."));
    ImGui::BeginDisabled(release_pending_.valid());
    if (ImGui::Button(tr("Check now"))) release_pending_ = std::async(std::launch::async, fetch_latest_release);
    ImGui::EndDisabled();
    // The answer, as a state: a glyph and its words.
    if (release_pending_.valid()) {
        ImGui::SameLine();
        weak(tr("asking GitHub..."));
    } else if (release_) {
        if (!release_->release) {
            state_row(anvil::ui::Glyph::Attention, release_->error);
        } else if (newer_release(release_->release->tag, ANVIL_VERSION)) {
            begin_banner("release", Tone::Info);
            ImGui::TextUnformatted(trf("Anvil {0} is available.", {release_->release->tag}).c_str());
            ImGui::SameLine();
            if (text_button(tr("Open its page"))) open_url(release_->release->url);
            end_banner(Tone::Info);
        } else {
            state_row(anvil::ui::Glyph::On, trf("You have the latest release ({0}).", {release_->release->tag}));
        }
    }
    ImGui::Spacing();
    ImGui::SeparatorText(tr("Third-party code"));
    weak_wrapped(tr("Dear ImGui, miniz, toml++ and Zydis (MIT License). Their license texts are in anvil/docs/third-party "
            "after installing."));
    weak_wrapped(tr("Chinese, Japanese and Korean text uses subsets of Source Han Sans by Adobe (SIL Open Font License "
                    "1.1, Reserved Font Name 'Source'). The license is in anvil/docs/third-party after installing."));
}

void ManagerApp::start_plan(bool uninstall) {
    const Installation& inst = *snapshot_->installation;
    PendingPlan pending;
    pending.uninstall = uninstall;
    pending.plan = uninstall ? plan_uninstall(*package_, inst, remove_everything_, restore_backup_ && inst.backup)
                             : plan_install(*package_, inst, game_, exe_, replace_foreign_dll_);
    plan_ = std::move(pending);
    open_plan_ = true;
}

void ManagerApp::run_plan() {
    const PendingPlan pending = std::move(*plan_);
    plan_.reset();
    if (game_running(game_)) {
        running_ = true;
        show_message(tr("Nothing changed"), tr("EU4 is running from this folder. Close it first."));
        return;
    }
    const ApplyResult result = apply_plan(*package_, pending.plan, game_, exe_);
    refresh();
    if (!result.ok) {
        failed(pending.uninstall ? tr("Anvil was not fully uninstalled") : tr("Anvil was not fully installed"),
               result.error + (result.done > 0 ? "\n\n" + std::string(tr("The steps before it are done; run it again once "
                                                                        "the cause is fixed."))
                                               : std::string()),
               result.access_denied);
        return;
    }
    if (pending.uninstall) {
        remove_link(start_menu_link(), game_ / kManagerExe);
        start_menu_game_.clear();
    }
    if (!pending.uninstall) {
        show_message(tr("Done"), trf("Anvil {0} is installed. Start the game to load your plugins.", {package_->version}));
    } else if (result.manager_left) {
        const bool scheduled = remove_after_exit(game_ / kManagerExe);
        show_message(tr("Done"), scheduled ? tr("Anvil is uninstalled. The Manager closes now and removes its copy in "
                                                "the game folder.")
                                           : tr("Anvil is uninstalled. Delete AnvilManager.exe in the game folder "
                                                "yourself once the Manager is closed."));
        close_after_message_ = true;
    } else {
        show_message(tr("Done"), tr("Anvil is uninstalled. The game starts vanilla."));
    }
}

void ManagerApp::open_zip(const fs::path& file) {
    if (read_only() || !snapshot_ || !snapshot_->report) {
        show_message(tr("Not now"), running_ ? tr("Close the game first.") : tr("Install Anvil first, then its plugins."));
        return;
    }
    std::string error;
    auto zip = ZipArchive::open_file(file, error);
    if (!zip) {
        show_message(tr("Not a plugin zip"), u8(file.filename()) + ": " + error);
        return;
    }
    check_zip(std::move(*zip), u8(file.filename()), nullptr);
}

void ManagerApp::check_zip(ZipArchive zip, std::string name, const CatalogPlugin* pin) {
    wchar_t temp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, temp);
    zip_check_ = check_plugin_zip(zip, game_, temp, package_->standard_plugins);
    if (pin != nullptr && zip_check_->error.empty()) zip_check_->error = check_pinned_manifest(*pin, zip_check_->id, zip_check_->version);
    zip_ = std::move(zip);
    zip_name_ = std::move(name);
    zip_anyway_ = false;
    open_zip_ = true;
}

void ManagerApp::downloaded(ZipDownload download) {
    const std::optional<CatalogPlugin> plugin = std::move(download_);
    download_.reset();
    if (!plugin) return;
    if (!download.error.empty()) {
        show_message(tr("The plugin was not installed"), zip_file_name(*plugin) + ": " + download.error);
        return;
    }
    if (read_only() || !snapshot_ || !snapshot_->report) {
        show_message(tr("Not now"), running_ ? tr("Close the game first.") : tr("Install Anvil first, then its plugins."));
        return;
    }
    std::string error;
    auto zip = ZipArchive::open(std::move(download.bytes), error);
    if (!zip) {
        show_message(tr("Not a plugin zip"), zip_file_name(*plugin) + ": " + error);
        return;
    }
    check_zip(std::move(*zip), zip_file_name(*plugin), &*plugin);
}

void ManagerApp::install_zip() {
    if (game_running(game_)) {
        running_ = true;
        show_message(tr("Nothing changed"), tr("EU4 is running from this folder. Close it first."));
        return;
    }
    const PluginInstallResult result = install_plugin_zip(*zip_, *zip_check_, game_);
    const std::string id = zip_check_->id;
    zip_.reset();
    zip_check_.reset();
    refresh();
    if (!result.ok) {
        failed(tr("The plugin was not installed"), result.error, result.access_denied);
        return;
    }
    show_message(tr("Done"), trf("{0} is installed in {1}. It loads when the game starts.",
                                 {id, "anvil/plugins/" + u8(result.folder.filename())}));
}

void ManagerApp::dropped(const fs::path& file) {
    if (_wcsicmp(file.extension().c_str(), L".zip") != 0) {
        show_message(tr("Not a zip"), tr("Drop a plugin's zip (<id>-<version>.zip) here to install it."));
        return;
    }
    open_zip(file);
}

void ManagerApp::show_message(std::string title, std::string text) {
    message_title_ = std::move(title);
    message_text_ = std::move(text);
    offer_elevation_ = false;
    open_message_ = true;
}

void ManagerApp::failed(const std::string& what, const std::string& error, bool access_denied) {
    show_message(what, error + (access_denied ? "\n\n" + std::string(tr("Windows refused access to the game folder. The "
                                                                        "Manager can start again as administrator and retry."))
                                              : std::string()));
    offer_elevation_ = access_denied;
}

void ManagerApp::draw_modals() {
    if (open_plan_) ImGui::OpenPopup("###plan");
    if (open_zip_) ImGui::OpenPopup("###zip");
    if (open_approve_) ImGui::OpenPopup("###approve");
    if (open_download_) ImGui::OpenPopup("###download");
    if (open_message_) ImGui::OpenPopup("###message");
    open_plan_ = open_zip_ = open_message_ = open_approve_ = open_download_ = false;
    draw_plan_modal();
    draw_zip_modal();
    draw_download_modal();
    draw_approve_modal();
    draw_plugins_confirm();

    next_modal(36);
    if (begin_dialog("###message", message_title_)) {
        wrapped(message_text_);
        if (offer_elevation_) {
            dialog_buttons({tr("Restart as administrator"), tr("OK")});
        } else {
            dialog_buttons({tr("OK")});
        }
        if (offer_elevation_ && primary_button(tr("Restart as administrator"))) {
            if (restart_elevated(game_)) close_ = true;
            ImGui::CloseCurrentPopup();
        }
        if (offer_elevation_) ImGui::SameLine();
        if (offer_elevation_ ? ImGui::Button(tr("OK")) : primary_button(tr("OK"))) {
            ImGui::CloseCurrentPopup();
            if (close_after_message_) close_ = true;
        }
        ImGui::EndPopup();
    }
}

void ManagerApp::draw_plan_modal() {
    next_modal(40);
    const bool same = snapshot_->installation->installed() && snapshot_->installation->dll_version == package_->version;
    const std::string title = plan_ && plan_->uninstall ? std::string(tr("Uninstall Anvil"))
                              : same                    ? trf("Repair Anvil {0}", {package_->version})
                                                        : trf("Install Anvil {0}", {package_->version});
    if (!begin_dialog("###plan", title)) return;
    if (!plan_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    // Uninstalling asks here what goes with Anvil; each answer plans again.
    if (plan_->uninstall) {
        const Installation& now = *snapshot_->installation;
        bool again = ImGui::Checkbox(tr("Also remove my plugins, their settings and data, crash reports and logs"), &remove_everything_);
        if (now.backup) {
            again |= ImGui::Checkbox(trf("Restore {0} from the backup", {now.backup->name}).c_str(), &restore_backup_);
            if (restore_backup_ && now.backup->name != "version.dll") {
                weak_wrapped(trf("Saves made with Mei may show garbled text in {0}.", {now.backup->name}));
            }
        }
        if (again) plan_->plan = plan_uninstall(*package_, now, remove_everything_, restore_backup_ && now.backup);
        ImGui::Spacing();
    }
    const Plan& plan = plan_->plan;
    if (!plan.refused.empty()) banner("refused", Tone::Bad, plan.refused);
    for (const std::string& note : plan.notes) bullet(note);
    if (plan.steps.empty() && plan.refused.empty()) wrapped(tr("Nothing to change."));
    if (!plan.steps.empty() &&
        ImGui::TreeNode("changes", "%s", trf("{0} change(s) in {1}", {std::to_string(plan.steps.size()), u8(game_)}).c_str())) {
        for (const Step& step : plan.steps) ImGui::BulletText("%s", i18n::format(step_text(step.kind), {step.path, step.to}).c_str());
        ImGui::TreePop();
    }
    const Installation& inst = *snapshot_->installation;
    const bool repair = inst.installed() && inst.dll_version == package_->version;
    const char* go = plan_->uninstall ? tr("Uninstall") : repair ? tr("Repair") : tr("Install");
    dialog_buttons({go, tr("Cancel")});
    ImGui::BeginDisabled(plan.steps.empty() || !plan.refused.empty() || read_only());
    if (primary_button(go)) {
        ImGui::CloseCurrentPopup();
        run_plan();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) {
        plan_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ManagerApp::draw_download_modal() {
    next_modal(40);
    const std::string title = download_ ? trf("Install {0}", {download_->name}) : std::string();
    if (!begin_dialog("###download", title)) return;
    if (!download_ || download_pending_.valid()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const CatalogPlugin& plugin = *download_;
    wrapped(trf("Download {0} ({1} MB) from {2}?", {zip_file_name(plugin), megabytes(plugin.zip.size), zip_source(plugin)}));
    weak_wrapped(trf("It is installed only if its SHA-256 is {0}, the one in the list. Before that you see what it holds and "
                     "what anvil check finds.",
                     {plugin.zip.sha256}));
    dialog_buttons({tr("Download"), tr("Cancel")});
    if (primary_button(tr("Download"))) {
        download_pending_ = std::async(std::launch::async, download_pinned_zip, plugin);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) {
        download_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ManagerApp::draw_zip_modal() {
    next_modal(40);
    if (!begin_dialog("###zip", tr("Install a plugin"))) return;
    if (!zip_check_) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const PluginZipCheck& check = *zip_check_;
    weak(zip_name_);
    bool can_install = check.error.empty();
    if (!check.error.empty()) {
        banner("error", Tone::Bad, trf("This zip cannot be installed: {0}", {check.error}));
    } else {
        const Manifest& m = *check.report.manifest;
        ImGui::Text("%s %s", m.name.empty() ? m.id.c_str() : m.name.c_str(), check.version.c_str());
        weak(m.authors.empty() ? trf("id {0}", {m.id}) : trf("id {0}, by {1}", {m.id, join(m.authors)}));
        if (!m.description.empty()) wrapped(plugin_text(m.folder, m.description));
        if (!m.repository.empty() && m.repository.starts_with("https://")) {
            if (ImGui::SmallButton(tr("Open its repository"))) open_url(m.repository);
        }
        ImGui::Spacing();
        banner("native", Tone::Warn, tr("A plugin is native code: it runs inside the game with your rights, like any "
                                        "program. Install plugins only from authors you trust."));
        // A module that changes the game counts, as in the approval of a plugin from a mod.
        if (changes_game(m)) {
            bullet(tr("It changes the game. Anvil keeps it out of ironman and multiplayer games unless you allow it there."));
        } else {
            bullet(tr("It says it does not change the game (affects_gameplay = false)."));
        }
        if (!check.installed_folder.empty()) {
            const std::string folder = "anvil/plugins/" + check.installed_folder;
            bullet(check.installed_version.empty()
                       ? trf("It replaces the installed copy in {0}. Its settings and data stay.", {folder})
                       : trf("It replaces version {0} in {1}. Its settings and data stay.", {check.installed_version, folder}));
        }
        if (!check.ignored.empty()) weak_wrapped(trf("Not installed (outside the plugin's folder in the zip): {0}", {join(check.ignored)}));
        const auto problems = shown_problems(check.report);
        if (!problems.empty()) {
            begin_banner("problems", Tone::Warn);
            for (const cli::Problem& problem : problems) {
                wrapped(problem.file + (problem.line > 0 ? ":" + std::to_string(problem.line) : std::string()) + ": " + problem.message);
            }
            end_banner(Tone::Warn);
        }
        if (check.would_skip) {
            begin_banner("skip", Tone::Bad);
            wrapped(trf("Anvil would not load it with this eu4.exe: {0}", {i18n::text(check.report.skip_why)}));
            ImGui::Checkbox(tr("Install it anyway"), &zip_anyway_);
            end_banner(Tone::Bad);
            can_install = zip_anyway_;
        } else if (check.report.problems.empty()) {
            state_row(anvil::ui::Glyph::On, tr("anvil check found no problems with this eu4.exe."));
        }
    }
    dialog_buttons({tr("Install"), tr("Cancel")});
    ImGui::BeginDisabled(!can_install || read_only());
    if (primary_button(tr("Install"))) {
        ImGui::CloseCurrentPopup();
        install_zip();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) {
        zip_.reset();
        zip_check_.reset();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace anvil::manager
