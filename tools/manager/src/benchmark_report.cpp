#include "benchmark_report.hpp"

#include "core/crash_history.hpp"
#include "files.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

namespace anvil::manager {
namespace {

namespace fs = std::filesystem;

Json optional_bool(const std::optional<bool>& v) { return v ? Json(*v) : Json(); }

template <class T> Json optional_value(const std::optional<T>& v) { return v ? Json(*v) : Json(); }

Json strings(const std::vector<std::string>& items) {
    Json out = Json::array();
    for (const std::string& s : items) out.push(s);
    return out;
}

// Decimal places: dividing by a power of ten gives the double nearest to the
// decimal, so it is written without a tail of digits.
double round_places(double value, double scale) { return std::round(value * scale) / scale; }

Json plugin_state(const RecordPlugin& p) {
    Json out = Json::object();
    out.set("state", p.state);
    if (p.state == "disabled") {
        out.set("by", p.by == "arm" ? "arm" : "player");
    } else if (p.state == "skipped") {
        out.set("reason", p.reason.empty() ? "other" : p.reason);
    } else if (p.state == "loaded") {
        out.set("participant", p.participant);
        if (p.participant) {
            out.set("done", p.done);
            out.set("learning", p.learning.empty() ? Json() : Json(p.learning));
            out.set("fields", p.fields.is_object() ? p.fields : Json::object());
        }
    }
    return out;
}

Json start_row(const StartRow& row) {
    Json out = Json::object();
    out.set("position", row.position);
    out.set("arm", arm_name(row.arm));
    out.set("time_ms", optional_value(row.time_ms));
    out.set("valid", row.valid);
    out.set("discard_reason", row.valid ? Json() : Json(row.discard_reason));
    out.set("killed", row.killed);
    Json cold = Json::object();
    cold.set("first_since_boot", optional_bool(row.first_since_boot));
    cold.set("definitions_changed", optional_bool(row.definitions_changed));
    if (row.open_cost) {
        Json cost = Json::object();
        cost.set("source", row.open_cost->from_plugin ? "plugin" : "game");
        cost.set("plugin", row.open_cost->from_plugin ? Json(row.open_cost->plugin) : Json());
        cost.set("files", row.open_cost->files);
        cost.set("ms", round_places(std::max(0.0, row.open_cost->ms), 1000.0));
        cold.set("open_cost", cost);
    } else {
        cold.set("open_cost", Json());
    }
    cold.set("class", row.valid ? Json(row.cold_class) : Json());
    out.set("cold_start", cold);
    out.set("input_during_start", optional_bool(row.input_during_start));
    if (row.crash) {
        Json crash = Json::object();
        crash.set("attribution", row.crash->attribution.empty() ? "no_plugin" : row.crash->attribution);
        crash.set("plugin", row.crash->plugin.empty() ? Json() : Json(row.crash->plugin));
        out.set("crash", crash);
    } else {
        out.set("crash", Json());
    }
    Json plugins = Json::object();
    for (const RecordPlugin& p : row.plugins) {
        // A plugin still "pending" at the record's last write never loaded:
        // the report knows only the four final states.
        if (p.state == "loaded" || p.state == "disabled" || p.state == "skipped" || p.state == "failed") {
            plugins.set(p.id, plugin_state(p));
        }
    }
    out.set("plugins", plugins);
    return out;
}

Json arm_summary(const ArmSummary& a) {
    Json out = Json::object();
    out.set("n", static_cast<int64_t>(a.times_ms.size()));
    out.set("median_ms", optional_value(a.median_ms));
    out.set("min_ms", optional_value(a.min_ms));
    out.set("max_ms", optional_value(a.max_ms));
    Json times = Json::array();
    for (const int64_t t : a.times_ms) times.push(t);
    out.set("times_ms", times);
    return out;
}

Json difference(const std::optional<Difference>& d, bool overlap) {
    if (!d) return Json();
    Json out = Json::object();
    out.set("ms", d->ms);
    out.set("percent", d->percent);
    if (overlap) out.set("ranges_overlap", d->ranges_overlap);
    return out;
}

Json result_json(const SeriesResult& r) {
    Json out = Json::object();
    out.set("computed_by", "manager");
    Json warm = Json::object();
    warm.set("without", arm_summary(r.warm[arm_index(Arm::Without)]));
    warm.set("with", arm_summary(r.warm[arm_index(Arm::With)]));
    out.set("warm", warm);
    out.set("difference", difference(r.difference, true));
    Json cold = Json::object();
    Json starts = Json::array();
    for (const ColdStart& c : r.cold) {
        Json s = Json::object();
        s.set("position", c.position);
        s.set("arm", arm_name(c.arm));
        s.set("time_ms", c.time_ms);
        s.set("kind", c.kind);
        starts.push(s);
    }
    cold.set("starts", starts);
    cold.set("difference", difference(r.cold_difference, false));
    out.set("cold", cold);
    Json not_counted = Json::array();
    for (const NotCounted& n : r.not_counted) {
        Json s = Json::object();
        s.set("position", n.position);
        s.set("reason", n.reason);
        not_counted.push(s);
    }
    out.set("not_counted", not_counted);
    Json input = Json::array();
    for (const int position : r.input_during_start) input.push(position);
    out.set("input_during_start", input);
    out.set("extra_starts_used", r.extra_starts_used);
    out.set("not_measured", strings(r.not_measured));
    return out;
}

// The declared fields of every participant, from the first start each took
// part in; labels stay in the preview.
Json plugin_schemas(const Series& series) {
    Json out = Json::object();
    for (const StartRow& row : series.rows()) {
        for (const RecordPlugin& p : row.plugins) {
            if (p.state != "loaded" || !p.participant || out.find(p.id) != nullptr) continue;
            Json schema = Json::object();
            for (const RecordField& f : p.schema) {
                Json field = Json::object();
                field.set("kind", f.kind);
                if (!f.unit.empty()) field.set("unit", f.unit);
                if (f.kind == "enum") field.set("options", strings(f.options));
                schema.set(f.name, field);
            }
            out.set(p.id, schema);
        }
    }
    return out;
}

Json versions_json(const ReportInput& in) {
    Json out = Json::object();
    Json game = Json::object();
    if (!in.game_build.empty()) {
        game.set("build", in.game_build);
    } else {
        std::string sha = in.game_sha256;
        std::transform(sha.begin(), sha.end(), sha.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        game.set("sha256", sha);
    }
    out.set("game", game);
    out.set("anvil", in.anvil_version);
    // Every plugin loaded in at least one arm, in the order they first loaded.
    struct Loaded {
        std::string id, version;
        bool arms[2] = {false, false};
    };
    std::vector<Loaded> loaded;
    for (const StartRow& row : in.series->rows()) {
        for (const RecordPlugin& p : row.plugins) {
            if (p.state != "loaded") continue;
            auto it = std::find_if(loaded.begin(), loaded.end(), [&](const Loaded& l) { return l.id == p.id; });
            if (it == loaded.end()) it = loaded.insert(loaded.end(), Loaded{p.id, p.version});
            it->arms[arm_index(row.arm)] = true;
        }
    }
    Json plugins = Json::array();
    for (const Loaded& l : loaded) {
        const auto source = std::find_if(in.plugins.begin(), in.plugins.end(), [&](const ReportPluginSource& s) { return s.id == l.id; });
        if (source == in.plugins.end()) continue;  // not one the Manager knows: no hash to report
        Json p = Json::object();
        p.set("id", l.id);
        p.set("version", l.version);
        p.set("source", source->source);
        p.set("workshop_item", source->source == "workshop" ? Json(source->workshop_item) : Json());
        p.set("sha256", source->sha256);
        Json arms = Json::array();
        if (l.arms[0]) arms.push("without");
        if (l.arms[1]) arms.push("with");
        p.set("arms", arms);
        plugins.push(p);
    }
    out.set("plugins", plugins);
    return out;
}

}  // namespace

std::string new_series_id() {
    unsigned char b[16] = {};
    BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);  // variant 10
    char text[40];
    std::snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                  b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return text;
}

Json benchmark_report(const ReportInput& in) {
    const Series& series = *in.series;
    Json out = Json::object();
    out.set("format", kReportFormat);
    out.set("series_id", in.series_id);
    Json s = Json::object();
    s.set("state", series.complete() ? "complete" : "stopped");
    s.set("stop_reason", series.complete() ? Json() : Json(series.stop_reason()));
    Json compared = Json::object();
    compared.set("id", series.plugin());
    compared.set("version", in.compared_version);
    s.set("compared", compared);
    s.set("target", series.target());
    out.set("series", s);
    Json starts = Json::array();
    for (const StartRow& row : series.rows()) starts.push(start_row(row));
    out.set("starts", starts);
    out.set("plugin_schemas", plugin_schemas(series));
    out.set("result", result_json(series_result(series)));

    Json system = Json::object();
    system.set("cpu", in.system.cpu);
    system.set("logical_cores", in.system.logical_cores);
    system.set("ram_gb", in.system.ram_gb);
    Json drives = Json::object();
    drives.set("game", in.system.drive_game);
    drives.set("user", in.system.drive_user);
    drives.set("workshop", in.system.drive_workshop);
    system.set("drives", drives);
    system.set("windows_build", in.system.windows_build);
    system.set("antivirus", strings(in.system.antivirus));
    out.set("system", system);

    Json mods = Json::object();
    mods.set("enabled", in.mods.enabled);
    mods.set("workshop_ids", in.include_workshop_ids ? strings(in.mods.workshop_ids) : Json());
    mods.set("local", in.mods.local);
    mods.set("files", in.mods.files);
    mods.set("size_mb", in.mods.size_mb);
    out.set("mods", mods);
    out.set("versions", versions_json(in));
    return out;
}

void set_workshop_ids(Json& report, const std::optional<std::vector<std::string>>& ids) {
    if (Json* mods = report.find("mods")) mods->set("workshop_ids", ids ? strings(*ids) : Json());
}

fs::path benchmarks_dir(const fs::path& game_dir) { return game_dir / "anvil" / "data" / "anvil" / "benchmarks"; }

std::string report_stamp() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond);
    return stamp;
}

fs::path sent_marker(const fs::path& file) {
    fs::path marker = file;
    return marker.replace_extension(".sent");
}

bool keep_report(const fs::path& dir, const std::string& stamp, const std::string& text, fs::path& file, std::string& error) {
    std::error_code ec;
    file = dir / (stamp + ".json");
    for (int i = 2; fs::exists(file, ec) || fs::exists(sent_marker(file), ec); ++i) {
        file = dir / (stamp + "-" + std::to_string(i) + ".json");
    }
    if (!write_file_atomic(file, text, error)) return false;
    // The stamps sort by time: everything before the newest kKeptReports goes.
    std::vector<fs::path> reports;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json") reports.push_back(entry.path());
    }
    std::sort(reports.begin(), reports.end(), [](const fs::path& a, const fs::path& b) { return a.filename() > b.filename(); });
    for (size_t i = kKeptReports; i < reports.size(); ++i) {
        fs::remove(reports[i], ec);
        fs::remove(sent_marker(reports[i]), ec);
    }
    return true;
}

bool rewrite_report(const fs::path& file, const std::string& text, std::string& error) {
    std::error_code ec;
    if (fs::exists(sent_marker(file), ec)) {
        error = "the report was sent; it stays as it was sent";
        return false;
    }
    return write_file_atomic(file, text, error);
}

bool mark_sent(const fs::path& file, std::string& error) {
    return write_file_atomic(sent_marker(file), "", error);
}

std::vector<KeptReport> kept_reports(const fs::path& dir) {
    std::vector<KeptReport> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
        KeptReport k;
        k.file = entry.path();
        const auto name = entry.path().stem().u8string();
        k.name.assign(name.begin(), name.end());
        k.sent = fs::exists(sent_marker(entry.path()), ec);
        std::string parse_error;
        const auto text = read_text_file(entry.path(), 1 << 20);
        const auto doc = text ? parse_json(*text, parse_error) : std::nullopt;
        if (doc && (*doc)["format"].is_integer() && (*doc)["format"].as_integer() == kReportFormat) {
            const Json& series = (*doc)["series"];
            k.readable = series["state"].is_string() && series["compared"]["id"].is_string();
            k.state = series["state"].is_string() ? series["state"].as_string() : "";
            k.stop_reason = series["stop_reason"].is_string() ? series["stop_reason"].as_string() : "";
            k.compared = series["compared"]["id"].is_string() ? series["compared"]["id"].as_string() : "";
            k.version = series["compared"]["version"].is_string() ? series["compared"]["version"].as_string() : "";
        }
        out.push_back(std::move(k));
    }
    std::sort(out.begin(), out.end(), [](const KeptReport& a, const KeptReport& b) { return a.name > b.name; });
    return out;
}

}  // namespace anvil::manager
