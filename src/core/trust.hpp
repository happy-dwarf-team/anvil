#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// One approval in anvil/trust.toml (design §14): the plugin id bound to the
// mod that provides it, and the folder hash the player approved.
struct TrustEntry {
    std::string id;
    std::string version;      // plugin.toml's version at the approval, for the player
    std::string mod;          // the mod's descriptor as dlc_load.json lists it ("mod/ugc_123.mod")
    std::string mod_name;
    std::string workshop_id;  // "" for a local mod
    std::string hash;         // hash_plugin_folder, lower-case hex
    std::string approved;     // UTC, "2026-10-20T18:05:11Z"
    uint32_t line = 0;        // of its [[plugin]] table
};

struct TrustFile {
    std::vector<TrustEntry> entries;  // at most one per id
    // The file does not parse (or breaks the schema): no plugin from a mod is
    // approved, and Anvil does not write the file until it is fixed or removed.
    std::string error;
    uint32_t line = 0;
    bool found = false;

    const TrustEntry* find(std::string_view id) const;
};

// [[plugin]] tables with every key above (approved: an offset date-time in
// UTC); an unknown key, a missing one, an id that is not a plugin
// id, a hash that is not 64 hex digits or the same id twice is an error.
TrustFile parse_trust_text(std::string_view text);
TrustFile read_trust(const std::filesystem::path& file);

// The file as Anvil writes it: a comment, then the entries by id.
std::string trust_text(std::vector<TrustEntry> entries);

// Writes `entries` atomically. Refuses (false, `error`) when the file on disk
// does not parse, so a hand edit with a mistake is never overwritten.
bool write_trust(const std::filesystem::path& file, const std::vector<TrustEntry>& entries, std::string& error);

// The current time in UTC, as `approved` holds it.
std::string utc_now();

}  // namespace anvil
