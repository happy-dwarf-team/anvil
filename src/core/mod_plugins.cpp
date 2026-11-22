#include "mod_plugins.hpp"

#include "discovery.hpp"
#include "files.hpp"
#include "log.hpp"
#include "plugin_hash.hpp"

#include <windows.h>

#include <algorithm>
#include <map>

namespace anvil {

namespace fs = std::filesystem;

namespace {

std::string names_of(const std::vector<const ModPlugin*>& plugins) {
    std::string out;
    for (const ModPlugin* p : plugins) out += (out.empty() ? "'" : ", '") + p->mod.name + "'";
    return out;
}

bool is_folder(const fs::path& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string version_of(const ModPlugin& p) { return p.manifest ? p.manifest->version.to_string() : std::string("?"); }

void set_reason(ModPlugin& p, i18n::Message why) {
    p.reason = i18n::english(why);
    p.why = std::move(why);
}

void set_problem(ModPlugin& p, i18n::Message why) {
    p.problem = i18n::english(why);
    p.problem_why = std::move(why);
}

// The plugin as the approval in its `approval` (bound to its mod) decides it.
void decide(ModPlugin& p, const fs::path& anvil_dir, const CopyExists& copy_exists) {
    const std::optional<TrustEntry>& e = p.approval;
    if (!p.bound) {
        if (!p.problem.empty()) {
            p.state = ModPluginState::Invalid;
            set_reason(p, p.problem_why);
        } else {
            p.state = ModPluginState::Pending;
            set_reason(p, e ? i18n::Message("approved_other_mod", {e->mod_name})
                            : i18n::Message("waiting_approval", {short_hash(p.hash)}));
        }
        return;
    }
    const fs::path copy = approved_copy(anvil_dir, p.id, e->hash);
    if (!p.hash.empty() && p.hash == e->hash) {
        p.state = ModPluginState::Approved;
        p.load_folder = copy;
        p.copy_missing = !copy_exists(p.id, e->hash);
        set_reason(p, i18n::Message("approved_on", {e->approved.substr(0, 10), short_hash(e->hash)}));
        return;
    }
    if (copy_exists(p.id, e->hash)) {
        p.state = ModPluginState::UpdatePending;
        p.load_folder = copy;
        set_reason(p, p.problem.empty()
                          ? i18n::Message("update_waits", {version_of(p), short_hash(p.hash), e->version})
                          : i18n::Message("update_invalid", {p.problem_why, e->version}));
        return;
    }
    if (!p.problem.empty()) {
        p.state = ModPluginState::Invalid;
        set_reason(p, i18n::Message("approved_gone", {p.problem_why, e->version}));
        return;
    }
    p.state = ModPluginState::Pending;
    set_reason(p, i18n::Message("changed_copy_missing", {e->version, short_hash(p.hash)}));
}

// Copies `folder` into its approved copy for `hash` and checks the copy.
bool copy_verified(const fs::path& anvil_dir, const std::string& id, const fs::path& folder, const std::string& hash,
                   std::string& error) {
    const FolderHash source = hash_plugin_folder(folder);
    if (!source.error.empty()) {
        error = source.error;
        return false;
    }
    if (source.sha256 != hash) {
        error = "the plugin's files changed (hash " + short_hash(source.sha256) + ", not " + short_hash(hash) +
                "); look at it again";
        return false;
    }
    const fs::path target = approved_copy(anvil_dir, id, hash);
    if (is_folder(target)) {
        if (hash_plugin_folder(target).sha256 == hash) return true;
        if (const DWORD code = remove_tree(target); code != 0) {
            error = "cannot replace the damaged copy " + narrow(target.wstring()) + " (error " + std::to_string(code) + ")";
            return false;
        }
    }
    const fs::path temporary = target.parent_path() / (L".new-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                                       std::to_wstring(GetTickCount64()));
    remove_tree(temporary);
    std::error_code ec;
    fs::create_directories(temporary, ec);
    if (ec) {
        error = "cannot create " + narrow(temporary.wstring()) + " (error " + std::to_string(ec.value()) + ")";
        return false;
    }
    const auto fail = [&](const std::string& message) {
        remove_tree(temporary);
        error = message;
        return false;
    };
    for (const HashedFile& file : source.files) {
        const fs::path to = temporary / file.path.lexically_relative(folder);
        fs::create_directories(to.parent_path(), ec);
        if (!CopyFileW(file.path.c_str(), to.c_str(), TRUE)) {
            return fail("cannot copy " + file.relative + " (error " + std::to_string(GetLastError()) + ")");
        }
    }
    const FolderHash copied = hash_plugin_folder(temporary);
    if (copied.sha256 != hash) return fail("the plugin's files changed while they were copied; try again");
    if (!MoveFileExW(temporary.c_str(), target.c_str(), 0)) {
        return fail("cannot move the copy to " + narrow(target.wstring()) + " (error " + std::to_string(GetLastError()) + ")");
    }
    return true;
}

// Removes approved/<id>/* except the copies named in `keep`; errors are left
// for the next start.
void remove_copies_except(const fs::path& anvil_dir, const std::string& id, const std::vector<std::string>& keep) {
    const fs::path dir = anvil_dir / "data" / "anvil" / "approved" / widen(id);
    std::error_code ec;
    std::vector<fs::path> children;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) children.push_back(it->path());
    for (const fs::path& child : children) {
        const std::string name = narrow(child.filename().wstring());
        if (std::find(keep.begin(), keep.end(), name) == keep.end()) remove_tree(child);
    }
    if (keep.empty() || std::all_of(keep.begin(), keep.end(), [](const std::string& k) { return k.empty(); })) {
        RemoveDirectoryW(dir.c_str());  // only when empty
    }
}

}  // namespace

const char* state_name(ModPluginState state) {
    switch (state) {
    case ModPluginState::Approved: return "approved";
    case ModPluginState::UpdatePending: return "update-pending";
    case ModPluginState::Pending: return "pending";
    case ModPluginState::Conflict: return "conflict";
    case ModPluginState::Shadowed: return "shadowed";
    case ModPluginState::Invalid: return "invalid";
    }
    return "?";
}

fs::path approved_copy(const fs::path& anvil_dir, const std::string& id, const std::string& hash) {
    return anvil_dir / "data" / "anvil" / "approved" / widen(id) / widen(hash);
}

std::string trust_warning(const TrustFile& trust) {
    if (trust.error.empty()) return {};
    return "anvil/trust.toml does not parse (" + trust.error +
           (trust.line ? ", line " + std::to_string(trust.line) : std::string()) +
           "): no plugin from a mod is approved until it is fixed or removed";
}

ModDiscovery find_mod_plugins(const fs::path& user_dir, const WhileHeld& while_held) {
    ModDiscovery d;
    d.playset = read_playset(user_dir);
    if (!d.playset.error.empty()) d.warnings.push_back(d.playset.error + "; no plugins from mods are loaded");
    for (const PlaysetMod& mod : d.playset.mods) {
        if (!mod.error.empty()) {
            // A zipped mod cannot carry plugins; it is no reason to warn.
            if (!mod.error.starts_with("a zipped mod")) d.warnings.push_back("mod " + mod.descriptor + ": " + mod.error);
            continue;
        }
        const fs::path plugins_dir = mod.folder / "anvil";
        if (!is_folder(plugins_dir)) continue;
        for (const fs::path& folder : plugin_folders(plugins_dir)) {
            if (is_mod_data_folder(narrow(folder.filename().wstring()))) continue;
            ModPlugin p;
            p.id = narrow(folder.filename().wstring());
            p.mod = mod;
            p.folder = folder;
            // The hash first, with the files held: what is read below is what it covers.
            const HeldFolder held(folder);
            if (!is_valid_plugin_id(p.id)) {
                set_problem(p, i18n::Message("bad_folder_id", {}));
            } else {
                const ManifestResult parsed = held.ok() ? load_manifest(held) : load_manifest(folder);
                if (!parsed.manifest) {
                    set_problem(p, "plugin.toml: " + parsed.error);
                } else if (parsed.manifest->id != p.id) {
                    set_problem(p, i18n::Message("id_mismatch", {parsed.manifest->id, p.id}));
                } else if (i18n::Message missing = missing_entry_reason(*parsed.manifest); !missing.empty()) {
                    set_problem(p, std::move(missing));
                }
                p.manifest = parsed.manifest;
            }
            p.hash = held.hash().sha256;
            if (!held.ok() && p.problem.empty()) set_problem(p, held.hash().error);
            if (while_held) while_held(p, held);
            d.plugins.push_back(std::move(p));
        }
    }
    return d;
}

void decide_mod_plugins(std::vector<ModPlugin>& plugins, const TrustFile& trust, const std::set<std::string>& local_ids,
                        const fs::path& anvil_dir, const CopyExists& copy_exists) {
    std::map<std::string, std::vector<ModPlugin*>> by_id;
    for (ModPlugin& p : plugins) {
        p.state = ModPluginState::Pending;
        p.reason.clear();
        p.why = {};
        p.approval.reset();
        p.bound = false;
        p.load_folder.clear();
        p.copy_missing = false;
        p.others.clear();
        if (!is_valid_plugin_id(p.id)) {
            p.state = ModPluginState::Invalid;
            set_reason(p, p.problem_why);
            continue;
        }
        by_id[p.id].push_back(&p);
    }
    for (auto& [id, group] : by_id) {
        const TrustEntry* entry = trust.find(id);
        ModPlugin* bound = nullptr;
        for (ModPlugin* p : group) {
            if (entry != nullptr) p->approval = *entry;
            p->bound = entry != nullptr && entry->mod == p->mod.descriptor;
            if (p->bound && bound == nullptr) bound = p;
        }
        const auto others_of = [&](const ModPlugin* self) {
            std::vector<const ModPlugin*> others;
            for (const ModPlugin* p : group) {
                if (p != self) others.push_back(p);
            }
            return others;
        };
        if (local_ids.contains(id)) {
            for (ModPlugin* p : group) {
                p->state = ModPluginState::Shadowed;
                set_reason(*p, i18n::Message("shadowed", {}));
                for (const ModPlugin* o : others_of(p)) p->others.push_back(o->mod.name);
            }
            continue;
        }
        if (group.size() == 1) {
            decide(*group.front(), anvil_dir, copy_exists);
            continue;
        }
        for (ModPlugin* p : group) {
            for (const ModPlugin* o : others_of(p)) p->others.push_back(o->mod.name);
            if (p == bound) {
                decide(*p, anvil_dir, copy_exists);
            } else if (bound != nullptr) {
                p->state = ModPluginState::Conflict;
                set_reason(*p, i18n::Message("conflict_bound", {bound->mod.name}));
            } else {
                p->state = ModPluginState::Conflict;
                set_reason(*p, i18n::Message("conflict_none", {std::to_string(group.size()), id, names_of(others_of(nullptr))}));
            }
        }
    }
}

ModDiscovery discover_mod_plugins(const fs::path& anvil_dir, const fs::path& user_dir,
                                  const std::set<std::string>& local_ids) {
    ModDiscovery d = find_mod_plugins(user_dir);
    d.trust = read_trust(anvil_dir / "trust.toml");
    if (const std::string warning = trust_warning(d.trust); !warning.empty()) {
        d.warnings.insert(d.warnings.begin() + (d.playset.error.empty() ? 0 : 1), warning);  // after the playset's
    }
    decide_mod_plugins(d.plugins, d.trust, local_ids, anvil_dir, [&](const std::string& id, const std::string& hash) {
        return is_folder(approved_copy(anvil_dir, id, hash));
    });
    return d;
}

bool copy_approved(const fs::path& anvil_dir, const std::string& id, const fs::path& folder, const std::string& hash,
                   std::string& error) {
    return copy_verified(anvil_dir, id, folder, hash, error);
}

bool approve_mod_plugin(const fs::path& anvil_dir, const ModPlugin& plugin, const std::string& hash,
                        const std::string& keep_hash, std::string& error) {
    if (!plugin.problem.empty()) {
        error = "cannot be approved: " + plugin.problem;
        return false;
    }
    if (!plugin.manifest) {
        error = "cannot be approved: no plugin.toml";
        return false;
    }
    if (hash.empty() || hash != plugin.hash) {
        error = "the plugin changed since it was shown; look at it again";
        return false;
    }
    const fs::path trust_file = anvil_dir / "trust.toml";
    const TrustFile trust = read_trust(trust_file);
    if (!trust.error.empty()) {
        error = "anvil/trust.toml does not parse (" + trust.error + "); fix or remove it first";
        return false;
    }
    if (!copy_verified(anvil_dir, plugin.id, plugin.folder, hash, error)) return false;
    std::vector<TrustEntry> entries;
    for (const TrustEntry& e : trust.entries) {
        if (e.id != plugin.id) entries.push_back(e);
    }
    TrustEntry e;
    e.id = plugin.id;
    e.version = plugin.manifest->version.to_string();
    e.mod = plugin.mod.descriptor;
    e.mod_name = plugin.mod.name;
    e.workshop_id = plugin.mod.workshop_id;
    e.hash = hash;
    e.approved = utc_now();
    entries.push_back(std::move(e));
    if (!write_trust(trust_file, entries, error)) return false;
    remove_copies_except(anvil_dir, plugin.id, {hash, keep_hash});
    return true;
}

bool revoke_mod_plugin(const fs::path& anvil_dir, const std::string& id, const std::string& keep_hash,
                       std::string& error) {
    const fs::path trust_file = anvil_dir / "trust.toml";
    const TrustFile trust = read_trust(trust_file);
    if (!trust.error.empty()) {
        error = "anvil/trust.toml does not parse (" + trust.error + "); fix or remove it first";
        return false;
    }
    if (trust.find(id) != nullptr) {
        std::vector<TrustEntry> entries;
        for (const TrustEntry& e : trust.entries) {
            if (e.id != id) entries.push_back(e);
        }
        if (!write_trust(trust_file, entries, error)) return false;
    }
    remove_copies_except(anvil_dir, id, {keep_hash});
    return true;
}

}  // namespace anvil
