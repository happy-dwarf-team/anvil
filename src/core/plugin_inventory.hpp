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
    // The check of a copy that may load in place of its mod's folder: the one
    // trust.toml approves for its id while no mod's folder of that id has its
    // hash (an update waits). A copy with a mod folder's hash has that
    // folder's check (PluginInventory::copy_check), and the copies of other
    // hashes, which the start removes, have none.
    std::optional<PluginCheck> check;
    // The copy trust.toml approves for its id no longer has its name's hash
    // (T4, #543): it does not count as there, so the start makes it again
    // from a mod folder that still has the approved hash, and without one
    // the plugin waits for approval. Other copies are not hashed.
    bool damaged = false;
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
    // The check of the approved copy `id`/`hash`: its own, else that of a mod's
    // folder with the same id and hash (the same files), else none.
    const PluginCheck* copy_check(const std::string& id, const std::string& hash) const;
    // Whether a plugin with this id is here at all: in anvil/plugins or in a mod.
    bool installed(const std::string& id) const;
};

// Reads anvil/plugins and the mods of `user_dir`'s playset, and checks each
// plugin against `game` (check_plugin_folder). Of the approved copies, it
// checks only one that may load in place of its mod's folder (above); it
// reads anvil/trust.toml for that alone, and approves nothing.
PluginInventory take_inventory(const std::filesystem::path& anvil_dir, const std::filesystem::path& user_dir,
                               const GameImage& game);

// Frees what only the loading of this start reads: each check's parsed
// symbols.toml and its verification, which the plugin set has copied by then.
// The inventory stays for the next start's plan (anvil.control), which reads
// plugin.toml, the skip and settings.toml of each check, not its symbols
// (#461: with Bellows, Tongs and Hamweard in the playset these were some
// 670 KB of the start-up's heap).
void release_symbols(PluginInventory& inventory);

}  // namespace anvil
