#pragma once

#include "benchmark_series.hpp"
#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::manager {

// The start-up benchmark report, format 1 (docs/spec/benchmark-v1.md §7): one
// JSON document per series. The local copy, the preview, what Send uploads
// and what Copy copies are the same bytes. The exact document is
// services/bench-receiver/schemas/report-format-1.schema.json.
inline constexpr int kReportFormat = 1;

// The computer, as the report describes it (§7, "System"). Drive kinds:
// nvme | sata_ssd | hdd | other | unknown.
struct ReportSystem {
    std::string cpu;  // the processor's model string
    int logical_cores = 1;
    int ram_gb = 1;   // whole GB
    std::string drive_game = "unknown";
    std::string drive_user = "unknown";
    std::string drive_workshop = "unknown";
    int windows_build = 0;
    std::vector<std::string> antivirus;  // Security Center's product names
};

// The enabled mods: counts only, and the Workshop ids unless the player
// leaves them out.
struct ReportMods {
    int enabled = 0;
    std::vector<std::string> workshop_ids;  // of the enabled Workshop mods
    int local = 0;
    uint64_t files = 0;
    uint64_t size_mb = 0;  // rounded
};

// Where a plugin that loaded came from: the report carries its source and the
// SHA-256 of the copy that loaded; its version and arms come from the rows.
struct ReportPluginSource {
    std::string id;
    std::string source = "local";  // local | workshop
    std::string workshop_item;     // the Workshop item for source workshop
    std::string sha256;            // lower-case hex
};

struct ReportInput {
    std::string series_id;  // new_series_id()
    const Series* series = nullptr;
    std::string compared_version;  // the compared plugin's version when the series started
    ReportSystem system;
    ReportMods mods;
    bool include_workshop_ids = true;  // the preview's only switch, on by default
    std::string game_build;            // a build Anvil knows, or ""
    std::string game_sha256;           // eu4.exe's, for an unknown build
    std::string anvil_version;
    std::vector<ReportPluginSource> plugins;  // every plugin that may have loaded
};

// A random UUID v4, new for every series; only used to drop a repeated Send.
std::string new_series_id();

// The report document, members in the schema's order.
Json benchmark_report(const ReportInput& input);
// The bytes that are kept, previewed, sent and copied.
inline std::string report_text(const Json& report) { return write_json(report, true); }

// The Workshop-id switch on a report that exists: mods.workshop_ids becomes
// `ids`, or null when the player leaves them out.
void set_workshop_ids(Json& report, const std::optional<std::vector<std::string>>& ids);

// ---- kept on the player's computer (§7) ---------------------------------------------

inline constexpr size_t kKeptReports = 10;

// <EU4>/anvil/data/anvil/benchmarks
std::filesystem::path benchmarks_dir(const std::filesystem::path& game_dir);

// The file name of a report finished now, local time: 2026-10-26T14-03-22.
std::string report_stamp();

// Writes `text` as <dir>/<stamp>.json in one step (another suffix when that
// name exists), then removes the oldest reports and their .sent markers
// beyond kKeptReports. `file` gets the path. False with `error`.
bool keep_report(const std::filesystem::path& dir, const std::string& stamp, const std::string& text,
                 std::filesystem::path& file, std::string& error);
// The Workshop-id switch rewrites the kept file; a sent report stays as it
// was sent (false with `error`).
bool rewrite_report(const std::filesystem::path& file, const std::string& text, std::string& error);
// The marker <name>.sent beside a report that was sent: the JSON stays byte
// for byte what was sent.
std::filesystem::path sent_marker(const std::filesystem::path& file);
bool mark_sent(const std::filesystem::path& file, std::string& error);

// A kept report for the setup screen's list.
struct KeptReport {
    std::filesystem::path file;
    std::string name;        // the stamp
    std::string compared;    // plugin id
    std::string version;     // its version
    std::string state;       // complete | stopped
    std::string stop_reason; // "" when complete
    bool sent = false;
    bool readable = false;   // the file is a report of a known format
};

// Newest first.
std::vector<KeptReport> kept_reports(const std::filesystem::path& dir);

}  // namespace anvil::manager
