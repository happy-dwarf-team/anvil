#include "plugin_inventory.hpp"

#include "discovery.hpp"
#include "log.hpp"
#include "trust.hpp"

#include <algorithm>

namespace anvil {

namespace fs = std::filesystem;

namespace {

std::vector<fs::path> sub_folders(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        if (it->is_directory(type_ec)) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

std::set<std::string> PluginInventory::local_ids() const {
    std::set<std::string> ids;
    for (const InventoryPlugin& p : local) ids.insert(p.id);
    return ids;
}

bool PluginInventory::copy_exists(const std::string& id, const std::string& hash) const { return copy(id, hash) != nullptr; }

const InventoryCopy* PluginInventory::copy(const std::string& id, const std::string& hash) const {
    for (const InventoryCopy& c : copies) {
        if (c.id == id && c.name == hash) return &c;
    }
    return nullptr;
}

const PluginCheck* PluginInventory::copy_check(const std::string& id, const std::string& hash) const {
    if (const InventoryCopy* c = copy(id, hash); c != nullptr && c->check) return &*c->check;
    for (const InventoryModPlugin& m : mods) {
        if (m.found.id == id && !m.found.hash.empty() && m.found.hash == hash) return &m.check;
    }
    return nullptr;
}

bool PluginInventory::installed(const std::string& id) const {
    for (const InventoryPlugin& p : local) {
        if (p.id == id) return true;
    }
    for (const InventoryModPlugin& p : mods) {
        if (p.found.id == id) return true;
    }
    return false;
}

void release_symbols(PluginInventory& inventory) {
    const auto release = [](PluginCheck& check) {
        check.symbols.reset();
        check.verification = SymbolVerification{};
    };
    for (InventoryPlugin& p : inventory.local) release(p.check);
    for (InventoryModPlugin& p : inventory.mods) release(p.check);
    for (InventoryCopy& c : inventory.copies) {
        if (c.check) release(*c.check);
    }
}

PluginInventory take_inventory(const fs::path& anvil_dir, const fs::path& user_dir, const GameImage& game) {
    PluginInventory inventory;
    inventory.anvil_dir = anvil_dir;
    inventory.user_dir = user_dir;
    inventory.plugins_dir = anvil_dir / "plugins";
    for (const fs::path& folder : plugin_folders(inventory.plugins_dir)) {
        InventoryPlugin p;
        p.folder = folder;
        p.check = check_plugin_folder(folder, game);
        p.id = p.check.manifest.manifest ? p.check.manifest.manifest->id : narrow(folder.filename().wstring());
        inventory.local.push_back(std::move(p));
    }

    ModDiscovery found = find_mod_plugins(user_dir);
    inventory.playset = std::move(found.playset);
    inventory.mod_warnings = std::move(found.warnings);
    for (ModPlugin& p : found.plugins) {
        InventoryModPlugin m;
        m.check = check_plugin_folder(p.folder, game);
        m.found = std::move(p);
        inventory.mods.push_back(std::move(m));
    }

    // The approved copies: what trust.toml's decisions and the prune read.
    // A start loads a copy in place of its mod's folder only while an update
    // waits: the copy of the hash trust.toml approves, which no mod's folder
    // of the id has. Only that one is checked.
    const TrustFile trust = read_trust(anvil_dir / "trust.toml");
    for (const fs::path& id_dir : sub_folders(anvil_dir / "data" / "anvil" / "approved")) {
        inventory.copy_ids.push_back(id_dir);
        const std::string id = narrow(id_dir.filename().wstring());
        for (const fs::path& folder : sub_folders(id_dir)) {
            InventoryCopy c;
            c.id = id;
            c.name = narrow(folder.filename().wstring());
            c.folder = folder;
            bool of_a_mod = false, same_as_a_mod = false;
            for (const InventoryModPlugin& m : inventory.mods) {
                if (m.found.id != id) continue;
                of_a_mod = true;
                same_as_a_mod = same_as_a_mod || (!m.found.hash.empty() && m.found.hash == c.name);
            }
            const TrustEntry* approved = trust.error.empty() ? trust.find(id) : nullptr;
            const bool may_load = approved != nullptr && approved->hash == c.name;
            if (of_a_mod && !same_as_a_mod && may_load) c.check = check_plugin_folder(folder, game);
            inventory.copies.push_back(std::move(c));
        }
    }
    return inventory;
}

}  // namespace anvil
