#pragma once

#include "manifest.hpp"
#include "playset.hpp"
#include "trust.hpp"

#include "i18n/message.hpp"
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace anvil {

// What happens to a plugin a mod ships in <mod>/anvil/<id>/ (design §14, #60).
enum class ModPluginState {
    Approved,       // its hash is approved for this mod: it loads from the approved copy
    UpdatePending,  // the mod has another hash: the previously approved copy loads until the new one is approved
    Pending,        // not approved (or approved for another mod): it does not load
    Conflict,       // another enabled mod has a plugin with this id and no approval decides: none loads
    Shadowed,       // a local plugin in anvil/plugins has this id and wins
    Invalid,        // cannot be approved: the folder or plugin.toml is wrong
};

const char* state_name(ModPluginState state);  // "approved", "update-pending", ...

struct ModPlugin {
    std::string id;                // the folder's name, which plugin.toml's id must equal
    PlaysetMod mod;
    std::filesystem::path folder;  // <mod>/anvil/<id>
    std::string hash;              // of `folder`; "" when it cannot be hashed
    std::optional<Manifest> manifest;  // of `folder`
    std::string problem;           // why `folder` cannot load or be approved; "" when it can
    ModPluginState state = ModPluginState::Pending;
    std::string reason;            // what the log and the player read: the English words of `why`
    i18n::Message problem_why, why;  // `problem` and `reason` by code, for the player's language (#158)
    // trust.toml's entry for this id; `bound` when it names this mod.
    std::optional<TrustEntry> approval;
    bool bound = false;
    // Approved and UpdatePending: anvil/data/anvil/approved/<id>/<approved hash>.
    std::filesystem::path load_folder;
    // Approved: the copy does not exist yet; the loader makes it from `folder`
    // (whose hash is the approved one) at the next start.
    bool copy_missing = false;
    std::vector<std::string> others;  // Conflict: the other mods' names; Shadowed: the local plugin's folder

    bool loads() const { return state == ModPluginState::Approved || state == ModPluginState::UpdatePending; }
};

struct ModDiscovery {
    Playset playset;
    TrustFile trust;
    std::vector<ModPlugin> plugins;    // by mod in playset order, then by id
    std::vector<std::string> warnings;  // dlc_load.json, descriptors, trust.toml
};

// anvil/data/anvil/approved/<id>/<hash>
std::filesystem::path approved_copy(const std::filesystem::path& anvil_dir, const std::string& id, const std::string& hash);

// The plugins of the mods enabled in `user_dir`'s playset, decided against
// anvil/trust.toml (design §14, #60). `local_ids`: the ids of anvil/plugins
// (folder names where plugin.toml is broken). Reads, never writes.
//  - A plugin whose id a local plugin has is Shadowed.
//  - Two or more enabled mods with the same id: the one trust.toml binds the
//    id to is decided as below and the others are Conflict; with none bound,
//    all of them are Conflict.
//  - Otherwise the entry for the id decides: bound to this mod with this hash
//    is Approved; bound with another hash is UpdatePending while that hash's
//    approved copy exists (Pending otherwise); anything else is Pending.
// A plugin with a `problem` is Invalid unless its previous approved copy
// still loads (UpdatePending).
ModDiscovery discover_mod_plugins(const std::filesystem::path& anvil_dir, const std::filesystem::path& user_dir,
                                  const std::set<std::string>& local_ids);

// Whether a folder of <mod>/anvil/ holds files for Anvil's standard plugins
// rather than a plugin, so discovery passes it by: map_modes (anvil-map's
// modes from files, docs/spec/map-mode-files-v1.md). A plugin id never has
// '_', so no plugin can need one of these names.
inline bool is_mod_data_folder(std::string_view name) { return name == "map_modes"; }

// The two halves of discover_mod_plugins. find_mod_plugins reads the
// playset and each plugin folder of its mods (id, mod, folder, hash,
// plugin.toml and why it cannot be approved) and decides nothing: `trust`
// stays empty and every state Pending. Each folder is hashed first and held
// (HeldFolder) while its plugin.toml is read from the hashed bytes, so the
// manifest is the one its hash names (T3, #545); `while_held` then gets the
// plugin and its held folder, for a check of the same bytes (the plugin
// inventory's). A folder whose hash failed has no hash to approve: its
// plugin.toml is read from the disk, for the player to see, and `while_held`
// gets that HeldFolder as it is (ok() false). decide_mod_plugins decides them as
// above against `trust`, without reading the disk: `copy_exists(id, hash)`
// says whether anvil/data/anvil/approved/<id>/<hash> is there.
using CopyExists = std::function<bool(const std::string& id, const std::string& hash)>;
class HeldFolder;
using WhileHeld = std::function<void(const ModPlugin& plugin, const HeldFolder& folder)>;
ModDiscovery find_mod_plugins(const std::filesystem::path& user_dir, const WhileHeld& while_held = {});
void decide_mod_plugins(std::vector<ModPlugin>& plugins, const TrustFile& trust, const std::set<std::string>& local_ids,
                        const std::filesystem::path& anvil_dir, const CopyExists& copy_exists);

// What the log and the player read when trust.toml does not parse; "" when it does.
std::string trust_warning(const TrustFile& trust);

// The approved copy of plugin `id` (a start plan's PlannedCopy), made from
// its mod's folder `folder` and checked against the approved `hash`. False
// with `error`.
bool copy_approved(const std::filesystem::path& anvil_dir, const std::string& id, const std::filesystem::path& folder,
                   const std::string& hash, std::string& error);

// Approves `plugin` as the player saw it (`hash`): checks that the mod folder
// still has that hash and can load, copies it to its approved copy, checks
// the copy, and binds the id to the plugin's mod in trust.toml (replacing an
// earlier approval of the id). Copies of the id other than this one and
// `keep_hash` (the copy the running game loaded) are removed where they can
// be. False with `error`; trust.toml is then unchanged.
bool approve_mod_plugin(const std::filesystem::path& anvil_dir, const ModPlugin& plugin, const std::string& hash,
                        const std::string& keep_hash, std::string& error);

// Removes the approval of `id` from trust.toml and its approved copies other
// than `keep_hash`. True also when there was none.
bool revoke_mod_plugin(const std::filesystem::path& anvil_dir, const std::string& id, const std::string& keep_hash,
                       std::string& error);

}  // namespace anvil
