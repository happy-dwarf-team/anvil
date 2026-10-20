#pragma once

#include "cli/check.hpp"
#include "zip.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace anvil::manager {

// A plugin zip the player picked (issue #59): `<id>-<version>.zip` as the
// template releases it, the contents of anvil/plugins/<id>/ in one folder.
// The plugin's folder in the zip is the one with the shallowest plugin.toml:
// the root, `<folder>/` or `anvil/plugins/<folder>/`. Nothing is downloaded.
struct PluginZipCheck {
    std::string error;  // the zip cannot be installed at all; the rest may be empty
    std::string root;   // the plugin's folder in the zip, "" or ending in '/'
    std::vector<const ZipEntry*> files;  // below root
    // The plugin as `anvil check <folder> --exe <EU4>/eu4.exe` sees it,
    // unpacked to a temporary folder (no symbols.lock is written).
    cli::CheckReport report;
    std::string id;       // from plugin.toml
    std::string version;  // from plugin.toml
    // The loader would skip it (report.skip_reason): the player confirms.
    bool would_skip = false;
    // anvil/plugins/<folder> that holds this id now, and its version.
    std::string installed_folder;
    std::string installed_version;  // empty when its plugin.toml does not parse
};

// Unpacks the plugin to a fresh folder below `temp_dir` and checks it
// against the game's eu4.exe; removes that folder again. A standard plugin
// (it comes with Anvil) and a zip without a plugin.toml are refused.
PluginZipCheck check_plugin_zip(const ZipArchive& zip, const std::filesystem::path& game_dir,
                                const std::filesystem::path& temp_dir,
                                const std::vector<std::string>& standard_plugins);

struct PluginInstallResult {
    bool ok = false;
    std::string error;
    bool access_denied = false;
    std::filesystem::path folder;  // where it went
};

// Installs a checked zip to anvil/plugins/<id>: unpacked first to
// anvil/data/anvil/manager/ (not a plugin folder the loader would find),
// then the old folder, if any, is moved aside, the new one moved in and the
// old one removed. On failure the old folder is put back. The player's data
// (anvil/data/<id>) stays.
PluginInstallResult install_plugin_zip(const ZipArchive& zip, const PluginZipCheck& check,
                                       const std::filesystem::path& game_dir);

// Removes anvil/plugins/<folder>; the player's data (anvil/data/<id>) stays.
// Standard plugins are refused.
bool remove_plugin_folder(const std::filesystem::path& game_dir, const std::string& folder,
                          const std::vector<std::string>& standard_plugins, std::string& error);

}  // namespace anvil::manager
