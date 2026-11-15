#pragma once

#include "mod_plugins.hpp"
#include "plugin_check.hpp"
#include "playset.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace anvil {

// A folder of anvil/plugins.
struct InventoryPlugin {
    std::filesystem::path folder;
    std::string id;  // plugin.toml's id; the folder's name when plugin.toml does not parse
    PluginCheck check;
};

// A plugin a mod ships, with the check of its folder in the mod.
struct InventoryModPlugin {
    ModPlugin found;  // as find_mod_plugins read it: not decided yet
    PluginCheck check;
};

// A folder anvil/data/anvil/approved/<id>/<name>: an approved copy, or what
// is left of one (an older hash, an unfinished copy).
struct InventoryCopy {
    std::string id;
    std::string name;  // the folder's name: the hash of a finished copy
    std::filesystem::path folder;
    // The check of a copy that may load in place of its mod's folder (one
    // whose hash no mod's folder of its id has: an update waits); a copy with
    // the folder's own hash has the folder's check.
    std::optional<PluginCheck> check;
};

// Plugin inventory (GLOSSARY, ADR 0014): what a game folder and its playset
// hold. Every plugin of anvil/plugins and of the playset's mods with its
// plugin.toml, the result of its check against the game (symbols,
// settings.toml, module names) and its settings; for a mod's plugin also
// what decides its approval (its hash and the approved copies on disk).
// Taking it reads the disk and the game image and writes nothing.
struct PluginInventory {
    std::filesystem::path anvil_dir;
    std::filesystem::path user_dir;
    std::filesystem::path plugins_dir;      // anvil/plugins
    std::vector<InventoryPlugin> local;     // in discovery order (plugin_folders)
    Playset playset;
    std::vector<std::string> mod_warnings;  // dlc_load.json, descriptors
    std::vector<InventoryModPlugin> mods;   // by mod in playset order, then by id
    std::vector<std::filesystem::path> copy_ids;  // anvil/data/anvil/approved/<id>
    std::vector<InventoryCopy> copies;

    // The local plugins' ids (folder names where plugin.toml does not parse),
    // which win over a mod's plugin with the same id (design §14).
    std::set<std::string> local_ids() const;
    bool copy_exists(const std::string& id, const std::string& hash) const;
    const InventoryCopy* copy(const std::string& id, const std::string& hash) const;
    // Whether a plugin with this id is here at all: in anvil/plugins or in a mod.
    bool installed(const std::string& id) const;
};

// Reads anvil/plugins and the mods of `user_dir`'s playset, and checks each
// plugin against `game` (check_plugin_folder).
PluginInventory take_inventory(const std::filesystem::path& anvil_dir, const std::filesystem::path& user_dir,
                               const GameImage& game);

}  // namespace anvil
