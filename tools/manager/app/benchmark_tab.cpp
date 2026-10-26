#include "benchmark_tab.hpp"

#include "core/crash_history.hpp"
#include "core/log.hpp"
#include "core/playset.hpp"
#include "ui.hpp"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace anvil::manager {
namespace {

namespace fs = std::filesystem;
using namespace ui;

// About one start: the time to ready, the quit and the pause.
constexpr int kSecondsPerStart = 55;

std::string seconds(double ms) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f s", ms / 1000.0);
    return text;
}

// "−7.4 s (−19.0 %)", with a minus sign, not a hyphen.
std::string signed_difference(double ms, double percent) {
    char text[64];
    std::snprintf(text, sizeof(text), "%s%.1f s (%s%.1f %%)", ms < 0 ? "\xE2\x88\x92" : "+", std::abs(ms) / 1000.0,
                  percent < 0 ? "\xE2\x88\x92" : "+", std::abs(percent));
    return text;
}

std::string text_of(const Json& v) {
    if (v.is_null()) return "not set";
    if (v.is_string()) return v.as_string();
    if (v.is_bool()) return v.as_bool() ? "yes" : "no";
    return write_json(v);
}

std::string plural(int64_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

// A ✓ or ✕ drawn in the line's height (the font has neither glyph).
void mark(bool ok, bool blocks) {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ok ? kGood : blocks ? kBad : kWarn);
    const float t = std::max(1.5f, h * 0.12f);
    if (ok) {
        draw->AddLine(ImVec2(p.x + h * 0.15f, p.y + h * 0.55f), ImVec2(p.x + h * 0.4f, p.y + h * 0.8f), color, t);
        draw->AddLine(ImVec2(p.x + h * 0.4f, p.y + h * 0.8f), ImVec2(p.x + h * 0.85f, p.y + h * 0.25f), color, t);
    } else {
        draw->AddLine(ImVec2(p.x + h * 0.2f, p.y + h * 0.25f), ImVec2(p.x + h * 0.8f, p.y + h * 0.85f), color, t);
        draw->AddLine(ImVec2(p.x + h * 0.8f, p.y + h * 0.25f), ImVec2(p.x + h * 0.2f, p.y + h * 0.85f), color, t);
    }
    ImGui::Dummy(ImVec2(h, h));
    ImGui::SameLine();
}

// Chips that wrap at the end of the line.
void chip(const std::string& label, const ImVec4& color, bool& first, float right_edge) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::CalcTextSize(label.c_str()).x + style.FramePadding.x * 2;
    if (!first) {
        const float right = ImGui::GetItemRectMax().x + style.ItemSpacing.x + width;
        if (right < right_edge) ImGui::SameLine();
    }
    first = false;
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_Button));
    ImGui::Button(label.c_str());
    ImGui::PopStyleColor(3);
}

const char* phase_words(RunPhase phase) {
    switch (phase) {
    case RunPhase::Starting: return "Starting eu4.exe";
    case RunPhase::Loading: return "Loading";
    case RunPhase::Waiting: return "Ready, waiting for plugins to finish";
    case RunPhase::Quitting: return "Quitting";
    case RunPhase::Pause: return "Pause before the next start";
    case RunPhase::Done: return "Done";
    }
    return "";
}

std::string not_measured_words(const std::string& item, const std::string& without, const std::string& with) {
    if (item == "time_to_ready") return "times run to anvil-game.ready, not to Running application";
    if (item == "no_cold_without") return "no cold start " + without;
    if (item == "no_cold_with") return "no cold start " + with;
    if (item == "below_target") return "fewer valid warm starts than the target";
    if (item == "stopped") return "the series was stopped";
    return item;
}

// The dot strip (§8): a row per arm on a shared axis, a dot per warm start,
// a mark at the median and a line from min to max.
void dot_strip(const Json& warm, const std::string& without, const std::string& with) {
    double lo = 1e18, hi = 0;
    for (const char* arm : {"without", "with"}) {
        for (const Json& t : warm[arm]["times_ms"].items()) {
            lo = std::min(lo, t.as_number());
            hi = std::max(hi, t.as_number());
        }
    }
    if (hi <= 0) return;
    const double pad = std::max(500.0, (hi - lo) * 0.08);
    lo -= pad;
    hi += pad;
    const float label_w = std::max(ImGui::CalcTextSize(without.c_str()).x, ImGui::CalcTextSize(with.c_str()).x) + 12;
    const float row_h = ImGui::GetTextLineHeight() * 1.6f;
    const float width = std::max(200.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float x0 = origin.x + label_w, x1 = origin.x + width - 8;
    const auto x_of = [&](double ms) { return x0 + static_cast<float>((ms - lo) / (hi - lo)) * (x1 - x0); };
    const ImU32 weak_color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 colors[2] = {ImGui::GetColorU32(ImVec4(0.65f, 0.70f, 0.78f, 1.0f)), ImGui::GetColorU32(ImVec4(0.45f, 0.66f, 0.98f, 1.0f))};
    int row = 0;
    for (const char* arm : {"without", "with"}) {
        const float y = origin.y + row_h * (static_cast<float>(row) + 0.5f);
        draw->AddText(ImVec2(origin.x, y - ImGui::GetTextLineHeight() / 2), ImGui::GetColorU32(ImGuiCol_Text),
                      row == 0 ? without.c_str() : with.c_str());
        const Json& a = warm[arm];
        if (a["min_ms"].is_number()) {
            draw->AddLine(ImVec2(x_of(a["min_ms"].as_number()), y), ImVec2(x_of(a["max_ms"].as_number()), y), colors[row], 2.0f);
            const float m = x_of(a["median_ms"].as_number());
            draw->AddLine(ImVec2(m, y - row_h * 0.38f), ImVec2(m, y + row_h * 0.38f), colors[row], 2.5f);
        }
        for (const Json& t : a["times_ms"].items()) draw->AddCircleFilled(ImVec2(x_of(t.as_number()), y), 4.5f, colors[row]);
        ++row;
    }
    const float axis_y = origin.y + row_h * 2 + 2;
    draw->AddLine(ImVec2(x0, axis_y), ImVec2(x1, axis_y), weak_color);
    for (const double ms : {lo + pad, hi - pad}) {
        const std::string label = seconds(ms);
        const float x = x_of(ms);
        draw->AddLine(ImVec2(x, axis_y), ImVec2(x, axis_y + 4), weak_color);
        draw->AddText(ImVec2(x - ImGui::CalcTextSize(label.c_str()).x / 2, axis_y + 5), weak_color, label.c_str());
    }
    ImGui::Dummy(ImVec2(width, row_h * 2 + ImGui::GetTextLineHeight() + 10));
}

}  // namespace

BenchmarkTab::BenchmarkTab(HWND window, ManagerSettings& settings) : window_(window), settings_(settings) {}

void BenchmarkTab::reset() {
    if (busy()) return;
    screen_ = Screen::Setup;
    checks_.clear();
    checked_report_ = nullptr;
    kept_.reset();
    report_.reset();
    series_.reset();
}

std::string BenchmarkTab::arm_words(const std::string& arm) const {
    const std::string id = report_ ? (*report_)["series"]["compared"]["id"].as_string() : progress_.series.plugin();
    return (arm == "with" ? "with " : "without ") + id;
}

void BenchmarkTab::draw(const BenchmarkContext& context) {
    if (game_ != context.game) {
        game_ = context.game;
        reset();
    }
    poll(context);
    ImGui::BeginChild("benchmark");
    switch (screen_) {
    case Screen::Setup: draw_setup(context); break;
    case Screen::Running: draw_running(); break;
    case Screen::Result: draw_result(); break;
    case Screen::Preview: draw_preview(); break;
    }
    ImGui::EndChild();
}

void BenchmarkTab::check(const BenchmarkContext& context) {
    checked_report_ = context.report;
    checked_plugin_ = plugin_;
    checks_ = setup_checks(read_setup_facts(context.game, *context.report, context.installed_anvil, plugin_));
}

void BenchmarkTab::draw_setup(const BenchmarkContext& context) {
    ImGui::Spacing();
    wrapped("The start-up benchmark measures how long EU4 takes to start on this computer without a plugin and with "
            "it. EU4 starts again and again, without the plugin and with it, and each start quits by itself. Nothing "
            "in your saves, mods or anvil.toml changes.");
    ImGui::Spacing();
    if (context.report == nullptr || !context.report->cannot_check.empty()) {
        colored_wrapped(kWarn, "Install Anvil on the Anvil tab first: the benchmark runs with Anvil and its plugins.");
        draw_kept(context);
        return;
    }
    const std::vector<ComparablePlugin> plugins = comparable_plugins(*context.report);
    if (plugins.empty()) {
        colored_wrapped(kWarn, "No plugin to compare: enable one on the Plugins tab, or approve one on the Mods tab.");
        draw_kept(context);
        return;
    }
    if (std::none_of(plugins.begin(), plugins.end(), [&](const ComparablePlugin& p) { return p.id == plugin_; })) {
        const auto bellows = std::find_if(plugins.begin(), plugins.end(), [](const ComparablePlugin& p) { return p.id == "bellows"; });
        plugin_ = bellows != plugins.end() ? bellows->id : plugins.front().id;
    }
    if (checked_report_ != context.report || checked_plugin_ != plugin_) check(context);

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    if (ImGui::BeginCombo("Compare", plugin_.c_str())) {
        for (const ComparablePlugin& p : plugins) {
            const std::string label = p.id + " " + p.version + (p.source == "workshop" ? " (Workshop)" : "");
            if (ImGui::Selectable(label.c_str(), p.id == plugin_)) plugin_ = p.id;
        }
        ImGui::EndCombo();
    }
    ImGui::TextUnformatted("Valid warm starts per arm:");
    for (const int t : kTargets) {
        ImGui::SameLine();
        if (ImGui::RadioButton(std::to_string(t).c_str(), target_ == t)) target_ = t;
    }
    ImGui::SameLine();
    weak("about " + std::to_string(2 * target_) + " starts, about " +
         std::to_string((2 * target_ * kSecondsPerStart + 59) / 60) + " minutes");

    ImGui::Spacing();
    ImGui::SeparatorText("Before the series");
    for (const SetupCheck& c : checks_) {
        mark(c.ok, c.blocks);
        ImGui::TextUnformatted(c.text.c_str());
        if (!c.ok) {
            ImGui::SameLine();
            weak(c.fix);
        }
    }
    ImGui::Spacing();
    wrapped("Please leave the computer alone while the series runs. Keyboard and mouse use is recorded next to each "
            "start and shown with the result.");
    ImGui::Spacing();
    ImGui::BeginDisabled(!checks_pass(checks_) || checks_.empty());
    if (ImGui::Button("Start the series")) start(context);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Check again")) check(context);
    draw_kept(context);
}

void BenchmarkTab::draw_kept(const BenchmarkContext& context) {
    if (!kept_) kept_ = kept_reports(benchmarks_dir(context.game));
    ImGui::Spacing();
    if (!ImGui::CollapsingHeader(("Kept reports (" + std::to_string(kept_->size()) + ")###kept").c_str())) return;
    if (ImGui::Button("Open the folder")) open_folder(benchmarks_dir(context.game));
    if (kept_->empty()) {
        weak("No reports yet. Each series keeps its report here; the last 10 are kept.");
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("kept", 5, flags)) return;
    ImGui::TableSetupColumn("Finished");
    ImGui::TableSetupColumn("Compared");
    ImGui::TableSetupColumn("Series");
    ImGui::TableSetupColumn("Sent", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (const KeptReport& k : *kept_) {
        ImGui::PushID(k.name.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(k.name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(k.readable ? (k.compared + " " + k.version).c_str() : "(not readable)");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(!k.readable ? "" : k.state == "complete" ? "complete" : ("stopped: " + k.stop_reason).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(k.sent ? "sent" : "");
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!k.readable);
        if (ImGui::SmallButton("Open")) {
            std::string error;
            const auto text = read_text_file(k.file, 1 << 20);
            if (auto doc = text ? parse_json(*text, error) : std::nullopt) {
                from_series_ = false;
                labels_.clear();
                show_report(std::move(*doc), k.file);
                report_text_ = *text;  // exactly the bytes kept
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void BenchmarkTab::start(const BenchmarkContext& context) {
    const std::vector<ComparablePlugin> plugins = comparable_plugins(*context.report);
    const auto compared = std::find_if(plugins.begin(), plugins.end(), [&](const ComparablePlugin& p) { return p.id == plugin_; });
    if (compared == plugins.end()) return;
    // The first arm alternates between series; only this computer remembers it.
    const Arm first = settings_.benchmark_first_arm == "without" ? Arm::With : Arm::Without;
    settings_.benchmark_first_arm = arm_name(first);
    std::string error;
    save_manager_settings(settings_, error);

    user_dir_ = context.report->user_dir.empty() ? eu4_user_dir(context.game) : fs::path(widen(context.report->user_dir));
    input_ = ReportInput{};
    input_.series_id = new_series_id();
    input_.compared_version = compared->version;
    input_.game_build = context.exe_build;
    input_.game_sha256 = context.exe_sha256;
    input_.anvil_version = context.installed_anvil;
    input_.plugins = plugin_sources(*context.report);

    RunnerConfig config;
    config.game_dir = context.game;
    config.exe = context.game / "eu4.exe";
    config.plugin = compared->id;
    config.target = target_;
    config.first = first;
    config.environment = [user = user_dir_, plugin = *compared] { return read_environment(user, plugin); };
    report_.reset();
    series_.reset();
    series_note_.clear();
    progress_ = RunProgress{};
    progress_.series = Series(compared->id, target_, first);
    runner_ = std::make_unique<SeriesRunner>(std::move(config));
    screen_ = Screen::Running;
}

void BenchmarkTab::poll(const BenchmarkContext& context) {
    using namespace std::chrono;
    if (runner_) {
        progress_ = runner_->progress();
        if (progress_.phase != RunPhase::Done) return;
        runner_.reset();
        series_ = progress_.series;
        if (!progress_.start_error.empty()) series_note_ = progress_.start_error + ".";
        else if (!progress_.environment_change.empty()) series_note_ = "What changed: " + progress_.environment_change + ".";
        if (series_->rows().empty()) {
            screen_ = Screen::Result;  // nothing ran: no report
            return;
        }
        // Reading the mods' folders and Security Center waits until no game start is measured.
        prepared_ = std::async(std::launch::async, [game = context.game, user = user_dir_] {
            return Prepared{read_system(game, user), read_mods(user)};
        });
    }
    if (prepared_.valid() && prepared_.wait_for(0s) == std::future_status::ready) {
        const Prepared prepared = prepared_.get();
        input_.series = &*series_;
        input_.system = prepared.system;
        input_.mods = prepared.mods;
        input_.include_workshop_ids = true;
        labels_.clear();
        for (const StartRow& row : series_->rows()) {
            for (const RecordPlugin& p : row.plugins) {
                for (const RecordField& f : p.schema) {
                    if (!f.label.empty()) labels_[p.id][f.name] = f.label;
                }
            }
        }
        Json report = benchmark_report(input_);
        fs::path file;
        keep_error_.clear();
        if (!keep_report(benchmarks_dir(context.game), report_stamp(), report_text(report), file, keep_error_)) file.clear();
        kept_.reset();
        from_series_ = true;
        show_report(std::move(report), file);
        workshop_ids_ = input_.mods.workshop_ids;
    }
}

void BenchmarkTab::show_report(Json report, fs::path file) {
    report_text_ = report_text(report);
    const Json& ids = report["mods"]["workshop_ids"];
    include_ids_ = !ids.is_null();
    workshop_ids_.reset();
    if (ids.is_array()) {
        std::vector<std::string> list;
        for (const Json& id : ids.items()) list.push_back(id.as_string());
        workshop_ids_ = std::move(list);
    }
    report_ = std::move(report);
    report_file_ = std::move(file);
    fields_start_.clear();
    copy_outcome_.clear();
    screen_ = Screen::Result;
}

void BenchmarkTab::switch_workshop_ids(bool include) {
    include_ids_ = include;
    set_workshop_ids(*report_, include ? workshop_ids_ : std::nullopt);
    report_text_ = report_text(*report_);
    if (!report_file_.empty()) {
        std::string error;
        if (!rewrite_report(report_file_, report_text_, error)) keep_error_ = error;
        kept_.reset();
    }
    copy_outcome_.clear();
}

void BenchmarkTab::draw_running() {
    const RunProgress& p = progress_;
    const Series& s = p.series;
    const std::string arm = arm_name(p.arm);
    ImGui::Spacing();
    if (p.phase == RunPhase::Done || prepared_.valid()) {
        wrapped("The series has ended. Making its report...");
        return;
    }
    const int expected = std::max(1, s.expected_starts());
    ImGui::Text("Start %d of about %d, %s", std::max(1, p.start), expected, arm_words(arm).c_str());
    ImGui::ProgressBar(static_cast<float>(s.rows().size()) / static_cast<float>(expected), ImVec2(-1, 0), "");
    std::string phase = phase_words(p.phase);
    if (p.phase == RunPhase::Loading) phase += " (" + std::to_string(static_cast<int>(p.seconds)) + " s)";
    if (p.phase == RunPhase::Waiting && !p.waiting_for.empty()) phase += ": " + join(p.waiting_for);
    weak(phase);

    ImGui::Spacing();
    ImGui::TextUnformatted("Valid warm starts per arm");
    for (const Arm a : {Arm::Without, Arm::With}) {
        const std::string label = std::to_string(s.warm(a)) + " / " + std::to_string(s.target());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        ImGui::ProgressBar(static_cast<float>(s.warm(a)) / static_cast<float>(s.target()), ImVec2(ImGui::GetFontSize() * 12, 0),
                           label.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(arm_words(arm_name(a)).c_str());
    }
    ImGui::Text("Extra starts used: %d / %d", s.extra_starts_used(), kMaxExtraStarts);

    if (!s.rows().empty()) {
        ImGui::Spacing();
        bool first = true;
        const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (const StartRow& row : s.rows()) {
            std::string label = "#" + std::to_string(row.position) + " " + (row.arm == Arm::Without ? "without" : "with");
            if (row.time_ms) label += " " + seconds(static_cast<double>(*row.time_ms));
            ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
            if (!row.valid) {
                label += " discarded";
                color = kBad;
            } else if (row.cold_class != "warm") {
                label += " " + row.cold_class;
                color = kWarn;
            }
            if (row.input_during_start.value_or(false)) label += " (input)";
            ImGui::PushID(row.position);
            chip(label, color, first, right_edge);
            ImGui::PopID();
        }
    }

    ImGui::Spacing();
    wrapped("Please leave the computer alone until the series ends. Closing the Manager loses the series; the "
            "running start still quits by itself.");
    ImGui::Spacing();
    if (p.stopping) {
        colored_wrapped(kWarn, "Stopping: this start finishes and quits cleanly, then the series ends.");
    } else if (ImGui::Button("Stop")) {
        runner_->stop();
    }
}

void BenchmarkTab::draw_result() {
    ImGui::Spacing();
    if (!report_) {
        if (prepared_.valid()) {
            wrapped("Making the report...");
            return;
        }
        // The series ended before its first start.
        colored_wrapped(kWarn, "The series ended before a start ran. " + series_note_);
        if (ImGui::Button("New series")) reset();
        return;
    }
    const Json& r = *report_;
    const Json& series = r["series"];
    const Json& result = r["result"];
    const std::string without = arm_words("without"), with = arm_words("with");
    if (series["state"].as_string() == "complete") {
        colored_wrapped(kGood, "Series complete.");
    } else {
        colored_wrapped(kWarn, "Series stopped: " + stop_reason_text(series["stop_reason"].as_string()));
        if (from_series_ && !series_note_.empty()) weak(series_note_);
    }
    weak(plural(static_cast<int64_t>(r["starts"].items().size()), "start", "starts") + ", " +
         std::to_string(result["extra_starts_used"].as_integer()) + " of " + std::to_string(kMaxExtraStarts) +
         " extra starts used, target " + std::to_string(series["target"].as_integer()) + " valid warm starts per arm.");

    ImGui::SeparatorText("Warm starts");
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("warm", 5, flags)) {
        ImGui::TableSetupColumn("Arm");
        ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Median");
        ImGui::TableSetupColumn("Min\xE2\x80\x93max");
        ImGui::TableSetupColumn("Every time", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableHeadersRow();
        for (const char* arm : {"without", "with"}) {
            const Json& a = result["warm"][arm];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(arm_words(arm).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%lld", static_cast<long long>(a["n"].as_integer()));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a["median_ms"].is_number() ? seconds(a["median_ms"].as_number()).c_str() : "-");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a["min_ms"].is_number()
                                       ? (seconds(a["min_ms"].as_number()) + " \xE2\x80\x93 " + seconds(a["max_ms"].as_number())).c_str()
                                       : "-");
            ImGui::TableNextColumn();
            std::vector<std::string> times;
            for (const Json& t : a["times_ms"].items()) times.push_back(seconds(t.as_number()));
            ImGui::TextUnformatted(join(times).c_str());
        }
        ImGui::EndTable();
    }
    dot_strip(result["warm"], without, with);

    const Json& d = result["difference"];
    if (d.is_object()) {
        const std::string line = "With " + series["compared"]["id"].as_string() + ": " +
                                 signed_difference(d["ms"].as_number(), d["percent"].as_number());
        if (d["ranges_overlap"].as_bool()) {
            ImGui::TextUnformatted(line.c_str());
            colored_wrapped(kWarn, "Ranges overlap: the difference is uncertain.");
        } else {
            colored_wrapped(d["ms"].as_number() < 0 ? kGood : kBad, line);
        }
    } else {
        weak("No difference: it needs a valid warm start in both arms.");
    }

    ImGui::SeparatorText("Cold starts");
    const Json& cold = result["cold"];
    if (cold["starts"].items().empty()) {
        weak("No cold start in this series.");
    } else if (ImGui::BeginTable("cold", 4, flags)) {
        ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Arm");
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableHeadersRow();
        for (const Json& c : cold["starts"].items()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("#%lld", static_cast<long long>(c["position"].as_integer()));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(arm_words(c["arm"].as_string()).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(seconds(c["time_ms"].as_number()).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(cold_kind_text(c["kind"].as_string()).c_str());
        }
        ImGui::EndTable();
    }
    if (cold["difference"].is_object()) {
        ImGui::TextUnformatted(("Cold, with " + series["compared"]["id"].as_string() + ": " +
                                signed_difference(cold["difference"]["ms"].as_number(), cold["difference"]["percent"].as_number()))
                                   .c_str());
    } else {
        weak("A cold difference needs a cold start in both arms.");
    }

    ImGui::SeparatorText("Starts that do not count");
    if (result["not_counted"].items().empty()) weak("None.");
    for (const Json& n : result["not_counted"].items()) {
        const int64_t position = n["position"].as_integer();
        std::string line = "#" + std::to_string(position) + ": " + not_counted_text(n["reason"].as_string());
        const Json& cost = r["starts"].items()[static_cast<size_t>(position - 1)]["cold_start"]["open_cost"];
        if (n["reason"].as_string() == "unclear" && cost.is_object() && cost["files"].as_integer() > 0) {
            char per_file[64];
            std::snprintf(per_file, sizeof(per_file), ", %.3f ms per file", cost["ms"].as_number() / cost["files"].as_number());
            line += per_file;
        }
        bullet(line);
    }
    if (!result["input_during_start"].items().empty()) {
        std::vector<std::string> positions;
        for (const Json& p : result["input_during_start"].items()) positions.push_back("#" + std::to_string(p.as_integer()));
        wrapped("Keyboard or mouse was used during " + join(positions) + "; those starts still count.");
    }
    std::vector<std::string> missing;
    for (const Json& item : result["not_measured"].items()) missing.push_back(not_measured_words(item.as_string(), without, with));
    weak_wrapped("Not measured: " + join(missing) + ".");

    ImGui::Spacing();
    if (ImGui::Button("Review the report and send...")) screen_ = Screen::Preview;
    ImGui::SameLine();
    if (ImGui::Button(from_series_ ? "New series" : "Back")) reset();
    if (!report_file_.empty()) {
        ImGui::SameLine();
        weak("Kept as " + u8(report_file_.filename()));
    }
    if (!keep_error_.empty()) colored_wrapped(kWarn, "The report could not be kept: " + keep_error_);
}

void BenchmarkTab::draw_preview() {
    const Json& r = *report_;
    const Json& result = r["result"];
    ImGui::Spacing();
    if (ImGui::SmallButton("< Result")) screen_ = Screen::Result;
    ImGui::SameLine();
    ImGui::TextUnformatted("The report, exactly as it is sent");

    ImGui::SeparatorText("Result");
    const Json& w = result["warm"];
    std::string line;
    for (const char* arm : {"without", "with"}) {
        line += (line.empty() ? "" : "; ") + arm_words(arm) + ": " +
                (w[arm]["median_ms"].is_number() ? "median " + seconds(w[arm]["median_ms"].as_number()) : std::string("no warm start")) +
                " (n = " + std::to_string(w[arm]["n"].as_integer()) + ")";
    }
    if (result["difference"].is_object()) {
        line += "; " + signed_difference(result["difference"]["ms"].as_number(), result["difference"]["percent"].as_number());
    }
    wrapped(line + ".");
    weak(plural(static_cast<int64_t>(r["starts"].items().size()), "start", "starts") + ", " +
         plural(static_cast<int64_t>(result["cold"]["starts"].items().size()), "cold start", "cold starts") + ", " +
         plural(static_cast<int64_t>(result["not_counted"].items().size()), "start that does not count",
                "starts that do not count") + ".");

    ImGui::SeparatorText("Your computer");
    const Json& s = r["system"];
    wrapped(s["cpu"].as_string() + ", " + std::to_string(s["logical_cores"].as_integer()) + " logical cores, " +
            std::to_string(s["ram_gb"].as_integer()) + " GB of memory");
    wrapped("Drives: game folder " + s["drives"]["game"].as_string() + ", EU4 user folder " + s["drives"]["user"].as_string() +
            ", Workshop folder " + s["drives"]["workshop"].as_string());
    std::vector<std::string> av;
    for (const Json& a : s["antivirus"].items()) av.push_back(a.as_string());
    wrapped("Windows build " + std::to_string(s["windows_build"].as_integer()) + "; antivirus: " + (av.empty() ? "none reported" : join(av)));

    ImGui::SeparatorText("Mods");
    const Json& m = r["mods"];
    wrapped(std::to_string(m["enabled"].as_integer()) + " enabled (" + std::to_string(m["local"].as_integer()) +
            " local, counted only), about " + std::to_string(m["files"].as_integer()) + " files and " +
            std::to_string(m["size_mb"].as_integer()) + " MB");
    const bool sent = !report_file_.empty() && fs::exists(sent_marker(report_file_));
    bool include = include_ids_;
    ImGui::BeginDisabled(sent || !workshop_ids_);
    if (ImGui::Checkbox("Include the Workshop ids of my mods", &include)) switch_workshop_ids(include);
    ImGui::EndDisabled();
    if (m["workshop_ids"].is_array()) {
        std::vector<std::string> ids;
        for (const Json& id : m["workshop_ids"].items()) ids.push_back(id.as_string());
        weak(ids.empty() ? "No Workshop mods." : join(ids));
    }

    ImGui::SeparatorText("Versions");
    const Json& v = r["versions"];
    wrapped("EU4: " + (v["game"]["build"].is_string() ? v["game"]["build"].as_string() : "unknown build, eu4.exe SHA-256 " +
                                                                                         v["game"]["sha256"].as_string()) +
            "; Anvil " + v["anvil"].as_string());
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("versions", 4, flags)) {
        ImGui::TableSetupColumn("Plugin");
        ImGui::TableSetupColumn("Version");
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("SHA-256", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableHeadersRow();
        for (const Json& p : v["plugins"].items()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p["id"].as_string().c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p["version"].as_string().c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p["source"].as_string() == "workshop" ? ("Workshop " + p["workshop_item"].as_string()).c_str() : "local");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p["sha256"].as_string().c_str());
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Plugin state in each start");
    std::vector<std::string> ids;
    for (const Json& start : r["starts"].items()) {
        for (const auto& [id, state] : start["plugins"].members()) {
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
    }
    const int columns = 1 + static_cast<int>(r["starts"].items().size());
    if (!ids.empty() && columns <= 64 &&
        ImGui::BeginTable("states", columns, flags | ImGuiTableFlags_ScrollX,
                          ImVec2(0, (ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2) *
                                            static_cast<float>(ids.size() + 1) +
                                        ImGui::GetStyle().ScrollbarSize + 2))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthFixed);
        for (const Json& start : r["starts"].items()) {
            ImGui::TableSetupColumn(("#" + std::to_string(start["position"].as_integer())).c_str(), ImGuiTableColumnFlags_WidthFixed);
        }
        ImGui::TableHeadersRow();
        for (const std::string& id : ids) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(id.c_str());
            for (const Json& start : r["starts"].items()) {
                ImGui::TableNextColumn();
                const Json& p = start["plugins"][id];
                if (p.is_null()) {
                    weak("-");
                    continue;
                }
                std::string cell = p["state"].as_string();
                if (cell == "disabled") cell += p["by"].as_string() == "arm" ? " (arm)" : " (you)";
                if (cell == "skipped") cell += ": " + p["reason"].as_string();
                if (p["participant"].as_bool() && !p["done"].as_bool()) cell += ", not finished";
                ImGui::TextUnformatted(cell.c_str());
            }
        }
        ImGui::EndTable();
    }

    for (const auto& [plugin, schema] : r["plugin_schemas"].members()) {
        ImGui::SeparatorText(("Fields reported by " + plugin).c_str());
        std::vector<int> starts;
        for (const Json& start : r["starts"].items()) {
            if (start["plugins"][plugin]["participant"].as_bool()) starts.push_back(static_cast<int>(start["position"].as_integer()));
        }
        if (starts.empty()) continue;
        int& shown = fields_start_[plugin];
        if (std::find(starts.begin(), starts.end(), shown) == starts.end()) shown = starts.front();
        ImGui::PushID(plugin.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::BeginCombo("Start", ("#" + std::to_string(shown)).c_str())) {
            for (const int position : starts) {
                if (ImGui::Selectable(("#" + std::to_string(position)).c_str(), position == shown)) shown = position;
            }
            ImGui::EndCombo();
        }
        const Json& entry = r["starts"].items()[static_cast<size_t>(shown - 1)]["plugins"][plugin];
        if (!entry["done"].as_bool()) {
            colored_wrapped(kWarn, "In this start " + plugin + " had not finished when the game quit: values it set later are missing.");
        }
        if (entry["learning"].is_string()) weak("Learning: " + entry["learning"].as_string());
        std::string group = "\x01";
        for (const auto& [name, field] : schema.members()) {
            const size_t dot = name.find('.');
            const std::string prefix = dot == std::string::npos ? "" : name.substr(0, dot);
            if (prefix != group) {
                group = prefix;
                if (!prefix.empty()) ImGui::TextUnformatted(prefix.c_str());
            }
            const auto plugin_labels = labels_.find(plugin);
            std::string label = name.substr(dot == std::string::npos ? 0 : dot + 1);
            if (plugin_labels != labels_.end() && plugin_labels->second.count(name) != 0) label = plugin_labels->second.at(name);
            std::string value = text_of(entry["fields"][name]);
            if (field["unit"].is_string() && !entry["fields"][name].is_null()) value += " " + field["unit"].as_string();
            ImGui::Indent();
            ImGui::TextUnformatted(label.c_str());
            ImGui::SameLine(ImGui::GetFontSize() * 16);
            ImGui::TextUnformatted(value.c_str());
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    weak_wrapped("Field labels appear only here, not in the report. Never in a report: computer and account names, paths, drive "
         "models and serial numbers, the GPU, free disk space, local mod names, dates and times of day, free text, log "
         "lines, crash reports, or any install or player identifier.");

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Show the JSON", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputTextMultiline("##json", &report_text_, ImVec2(-1, ImGui::GetTextLineHeight() * 18), ImGuiInputTextFlags_ReadOnly);
        weak("These are exactly the bytes kept on this computer, sent and copied.");
    }
    draw_send();
}

void BenchmarkTab::draw_send() {
    char size[32];
    std::snprintf(size, sizeof(size), "%.1f KB", static_cast<double>(report_text_.size()) / 1024.0);
    ImGui::Spacing();
    if (ImGui::Button((std::string("Copy (") + size + ")").c_str())) {
        copy_outcome_ = copy_to_clipboard(window_, report_text_) ? "Copied. Paste it into an issue or a message."
                                                                 : "The clipboard could not be opened.";
    }
    if (!copy_outcome_.empty()) weak(copy_outcome_);
}

}  // namespace anvil::manager
