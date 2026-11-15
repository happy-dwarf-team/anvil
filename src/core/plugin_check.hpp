#pragma once

#include "manifest.hpp"
#include "pe_image.hpp"
#include "resolver.hpp"
#include "settings.hpp"
#include "symbols.hpp"

#include "i18n/message.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil {

// The game a plugin is checked against (design §7): the running eu4.exe,
// read through OriginalImageReader so bytes a plugin already changed read in
// their original form, or an eu4.exe file through FileImageReader (anvil
// check, the Manager). The difference between the two stays in the reader.
struct GameImage {
    const ImageReader* reader = nullptr;  // null: no game (anvil check without --exe): files are parsed only
    std::optional<PeImage> image;         // parsed from `reader`; empty with `image_error`
    std::string image_error;
    const char* build_id = nullptr;       // the build this Anvil knows it as; null: unknown
    // Why a plugin with symbols.toml is skipped on an unknown build (design §6).
    i18n::Message unknown_build = i18n::Message("build_unknown", {});
    // For the diagnostics: the file's name and, when known, its SHA-256.
    std::string exe_name = "eu4.exe";
    std::string exe_sha256;
};

// `reader` parsed as a PE image, with the build id and the reason for an
// unknown build.
GameImage game_image(const ImageReader& reader, const char* build_id, i18n::Message unknown_build);

// A problem in one of a plugin's files: `file` is its path inside the plugin
// folder ("plugin.toml", "symbols.toml" or the name plugin.toml gives),
// `line` 1-based or 0 for the whole file.
struct PluginProblem {
    std::string file;
    uint32_t line = 0;
    std::string message;
};

// What one plugin folder is, checked against the game (ADR 0014, point 2):
// its plugin.toml, whether the loader may load it, the artefacts the check
// verified, and the diagnostics. The loader, anvil check and the Manager use
// this one check; anvil check adds only its checks for authors on top.
struct PluginCheck {
    ManifestResult manifest;         // plugin.toml as it parsed
    bool manifest_missing = false;   // there is no plugin.toml
    // The entry DLL plugin.toml names is not in the folder. The loader skips
    // such a plugin as it discovers it, before the player's choices count.
    i18n::Message entry_missing;
    // Why the loader skips the plugin on its own account, the first of: a
    // broken plugin.toml, a broken symbols.toml, a game build it does not
    // declare or that is unknown, a symbol that does not match, a broken
    // settings.toml or a `module` it does not declare. Empty: the plugin may
    // load as far as its own files go.
    i18n::Message skip;
    // The first file plugin.toml names that could not be read: its path inside
    // the folder. An unreadable symbols.toml leaves settings.toml checked; the
    // check stops at an unreadable settings.toml.
    std::string unreadable;
    // symbols.toml: parsed, the game's build selected when the file declares
    // it (else its first), and with a game every symbol verified. The loader
    // adopts the verified bytes (adopt_verified_bytes) into its own copy when
    // it builds the plugin's symbols.
    std::optional<SymbolFile> symbols;
    bool build_declared = false;   // the file declares the game's build
    bool verified = false;         // a game was there and every symbol matched it
    SymbolVerification verification;
    // settings.toml, parsed (also when its `module` keys are wrong; `skip` says so).
    std::optional<SettingsFile> settings;
    bool settings_fit = false;  // `settings` parsed and every `module` key names a module
    // Everything the loader acts on, in file order: what `skip` names and
    // every other failing symbol.
    std::vector<PluginProblem> problems;

    bool loadable() const { return manifest.manifest && entry_missing.empty() && skip.empty() && unreadable.empty(); }
};

// The one check of a plugin folder (ADR 0014): parses plugin.toml,
// symbols.toml and settings.toml with the loader's strict parsers and, with
// a game, selects its build and verifies every symbol. Reads, never writes.
// Without a game (`game.reader` null) only the files are parsed and `skip`
// says nothing about symbols.
PluginCheck check_plugin_folder(const std::filesystem::path& folder, const GameImage& game);

}  // namespace anvil
