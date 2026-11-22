// The plugin inventory (ADR 0014) on folders laid out on disk: what
// take_inventory reads and checks, and what a start plan made from it loads.
// Part of anvil_tests.
#include "core/log.hpp"
#include "core/mod_plugins.hpp"
#include "core/plugin_hash.hpp"
#include "core/plugin_inventory.hpp"
#include "core/start_plan.hpp"
#include "core/trust.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace anvil;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

// One folder per process: a second run of these tests at the same time does not share it.
fs::path fresh(const char* name) {
    const fs::path dir = fs::temp_directory_path() / (std::string(name) + "-" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    fs::create_directories(dir);
    return dir;
}

// A mod `name` in user/mod with the plugin `id` of `version`; returns the plugin's folder.
fs::path mod_plugin(const fs::path& user, const std::string& name, const std::string& id, const std::string& version) {
    const fs::path folder = user / "mod" / widen(name) / "anvil" / widen(id);
    write(folder / "plugin.toml", plugin_toml(id, "", version));
    write(folder / (id + ".dll"), "MZ");
    write(user / "mod" / widen(name + ".mod"), "name=\"" + name + "\"\npath=\"mod/" + name + "\"\n");
    return folder;
}

void enable_mods(const fs::path& user, const std::vector<std::string>& names) {
    std::string list;
    for (const std::string& n : names) list += (list.empty() ? "\"mod/" : ",\"mod/") + n + ".mod\"";
    write(user / "dlc_load.json", "{\"enabled_mods\":[" + list + "]}");
}

TrustEntry approval(const std::string& mod, const std::string& id, const std::string& version, const std::string& hash) {
    TrustEntry e;
    e.id = id;
    e.version = version;
    e.mod = "mod/" + mod + ".mod";
    e.mod_name = mod;
    e.hash = hash;
    e.approved = "2026-11-15T10:00:00Z";
    return e;
}

void copy_folder(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

}  // namespace

void plugin_inventory_tests() {
    run("plugin inventory: an update waits while another mod has the approved hash: the copy loads with that check", [] {
        const fs::path dir = fresh("anvil-inventory-update-other-mod");
        const fs::path anvil = dir / "anvil";
        const fs::path user = dir / "user";
        // One updated x to 2.0.0; Two still ships the 1.0.0 the player approved from One.
        mod_plugin(user, "One", "x", "2.0.0");
        const fs::path old = mod_plugin(user, "Two", "x", "1.0.0");
        enable_mods(user, {"One", "Two"});
        const std::string h1 = hash_plugin_folder(old).sha256;
        copy_folder(old, approved_copy(anvil, "x", h1));
        std::string error;
        CHECK(write_trust(anvil / "trust.toml", {approval("One", "x", "1.0.0", h1)}, error));

        const PluginInventory inventory = take_inventory(anvil, user, GameImage{});
        const StartPlan plan = plan_start(inventory, read_player_choices(inventory), {});
        CHECK(plan.mods.size() == 2 && plan.mods[0].state == ModPluginState::UpdatePending &&
              plan.mods[1].state == ModPluginState::Conflict);
        const PlannedPlugin* x = plan.find("x");
        CHECK(x != nullptr && x->not_loading == NotLoading::None && x->folder == approved_copy(anvil, "x", h1));
        // The copy's plugin.toml, not the update's: the DLL that loads is 1.0.0's.
        CHECK(x != nullptr && x->manifest && x->manifest->version.to_string() == "1.0.0");
        const PluginCheck* check = x != nullptr ? check_of(inventory, plan, *x) : nullptr;
        CHECK(check != nullptr && check->manifest.manifest && check->manifest.manifest->version.to_string() == "1.0.0");
        fs::remove_all(dir);
    });

    run("plugin inventory: an approved copy of a hash trust.toml no longer names is not checked", [] {
        const fs::path dir = fresh("anvil-inventory-stale-copy");
        const fs::path anvil = dir / "anvil";
        const fs::path user = dir / "user";
        const fs::path now = mod_plugin(user, "One", "x", "2.0.0");
        enable_mods(user, {"One"});
        const std::string h2 = hash_plugin_folder(now).sha256;
        // The copy of 1.0.0, from before the player approved the update.
        const fs::path source = dir / "x-1.0.0";
        write(source / "plugin.toml", plugin_toml("x", "", "1.0.0"));
        write(source / "x.dll", "MZ");
        const std::string h1 = hash_plugin_folder(source).sha256;
        const fs::path stale = approved_copy(anvil, "x", h1);
        copy_folder(source, stale);
        std::string error;
        CHECK(write_trust(anvil / "trust.toml", {approval("One", "x", "2.0.0", h2)}, error));

        const PluginInventory inventory = take_inventory(anvil, user, GameImage{});
        CHECK(inventory.copies.size() == 1 && inventory.copies[0].folder == stale && !inventory.copies[0].check);
        const StartPlan plan = plan_start(inventory, read_player_choices(inventory), {});
        CHECK(plan.writes.remove.size() == 1 && plan.writes.remove[0] == stale);
        // The copy trust.toml names while the mod has another hash is checked.
        CHECK(write_trust(anvil / "trust.toml", {approval("One", "x", "1.0.0", h1)}, error));
        const PluginInventory waits = take_inventory(anvil, user, GameImage{});
        CHECK(waits.copies.size() == 1 && waits.copies[0].check.has_value() && !waits.copies[0].damaged);
        fs::remove_all(dir);
    });

    run("plugin inventory: a damaged approved copy is made again from an unchanged mod, else waits for approval (T4, #543)", [] {
        const fs::path dir = fresh("anvil-inventory-damaged-copy");
        const fs::path anvil = dir / "anvil";
        const fs::path user = dir / "user";
        const fs::path in_mod = mod_plugin(user, "One", "x", "1.0.0");
        enable_mods(user, {"One"});
        const std::string h1 = hash_plugin_folder(in_mod).sha256;
        const fs::path copy = approved_copy(anvil, "x", h1);
        copy_folder(in_mod, copy);
        std::string error;
        CHECK(write_trust(anvil / "trust.toml", {approval("One", "x", "1.0.0", h1)}, error));
        const auto plan_of = [&] {
            const PluginInventory inventory = take_inventory(anvil, user, GameImage{});
            return std::make_pair(inventory.copies.size() == 1 && inventory.copies[0].damaged,
                                  plan_start(inventory, read_player_choices(inventory), {}));
        };
        {
            const auto [damaged, plan] = plan_of();
            CHECK(!damaged && plan.mods.size() == 1 && !plan.mods[0].copy_missing && plan.writes.copies.empty());
        }
        // A file of the copy changed (a disk error, a tool): the plan makes it again from the mod.
        write(copy / "x.dll", "MZ damaged");
        {
            const auto [damaged, plan] = plan_of();
            CHECK(damaged && plan.mods.size() == 1 && plan.mods[0].state == ModPluginState::Approved);
            CHECK(plan.mods[0].copy_missing && plan.writes.copies.size() == 1 && plan.writes.copies[0].hash == h1);
            const PlannedPlugin* x = plan.find("x");
            CHECK(x != nullptr && x->not_loading == NotLoading::None && x->makes_copy);
        }
        // The mod changed too: nothing has the approved files, so it waits for approval (Approve shows).
        write(in_mod / "README.txt", "2.0");
        {
            const auto [damaged, plan] = plan_of();
            CHECK(damaged && plan.mods.size() == 1 && plan.mods[0].state == ModPluginState::Pending);
            const PlannedPlugin* x = plan.find("x");
            CHECK(x != nullptr && x->not_loading == NotLoading::WaitsForApproval && x->awaiting_approval);
            CHECK(plan.writes.copies.empty());
        }
        fs::remove_all(dir);
    });

    run("plugin inventory: a symbols.toml that cannot be read leaves settings.toml checked and its retired values moved", [] {
        // symbols.toml is there (plugin.toml parses) but another program holds it open without
        // sharing it (an antivirus scan, an editor): it cannot be read.
        const fs::path dir = fresh("anvil-inventory-unreadable-symbols");
        const fs::path anvil = dir / "anvil";
        const fs::path lode = anvil / "plugins" / "lode";
        write(lode / "plugin.toml", plugin_toml("lode", "symbols = \"symbols.toml\"\nsettings = \"settings.toml\"\n\n"
                                                        "[[modules]]\nid = \"flows\"\nname = \"Flows\"\naffects_gameplay = false\n"));
        write(lode / "lode.dll", "MZ");
        write(lode / "settings.toml", "[retired.flows_mode]\nmodule = \"flows\"\noff = [\"off\"]\n");
        write(anvil / "data" / "lode" / "settings.toml", "flows_mode = \"off\"\n");
        write(lode / "symbols.toml", "");
        const HANDLE held = CreateFileW((lode / "symbols.toml").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(held != INVALID_HANDLE_VALUE);

        const PluginInventory inventory = take_inventory(anvil, dir / "user", GameImage{});
        CHECK(inventory.local.size() == 1);
        const PluginCheck& check = inventory.local[0].check;
        CHECK(check.unreadable == "symbols.toml" && check.skip.code == "cannot_read" && !check.loadable());
        CHECK(check.settings.has_value() && check.settings_fit);
        const StartPlan plan = plan_start(inventory, read_player_choices(inventory), {});
        CHECK(plan.load.empty() && plan.writes.retired.size() == 1 && plan.writes.retired[0].plugin == "lode");
        CHECK(plan.lists.disabled_modules == std::vector<std::string>{"lode.flows"});
        CloseHandle(held);
        fs::remove_all(dir);
    });

    run("plugin inventory: entry, symbols and settings names outside the ANSI code page are read as UTF-8", [] {
        // Japanese names fit no Western or Central European code page: read through it,
        // the entry was not found and generic_string() threw out of the whole start-up.
        const std::string entry = "\xE3\x83\x97\xE3\x83\xA9\xE3\x82\xB0.dll";      // プラグ.dll
        const std::string settings = "\xE8\xA8\xAD\xE5\xAE\x9A.toml";               // 設定.toml
        const std::string symbols = "\xE8\xA8\x98\xE5\x8F\xB7.toml";                // 記号.toml
        const fs::path dir = fresh("anvil-inventory-utf8-names");
        const fs::path anvil = dir / "anvil";
        const fs::path ja = anvil / "plugins" / "ja";
        const auto u8 = [](const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); };
        write(ja / "plugin.toml", "id = \"ja\"\nname = \"ja\"\nversion = \"1.0.0\"\napi = 2\nentry = \"" + entry +
                                      "\"\nphase = \"main\"\naffects_gameplay = false\nsettings = \"" + settings +
                                      "\"\nsymbols = \"" + symbols + "\"\n");
        write(ja / u8(entry), "MZ");
        write(ja / u8(settings), "[setting.speed]\ntype = \"int\"\ndefault = 3\nlabel = \"Speed\"\n");
        write(ja / u8(symbols), "builds = [\"eu4-1.37.5-steam-win64\"]\n");

        const PluginInventory inventory = take_inventory(anvil, dir / "user", GameImage{});
        CHECK(inventory.local.size() == 1);
        if (inventory.local.size() == 1) {
            const PluginCheck& check = inventory.local[0].check;
            CHECK(check.manifest.manifest.has_value());
            CHECK(check.entry_missing.empty());
            CHECK(check.unreadable.empty());
            CHECK(check.settings.has_value() && check.settings->settings.size() == 1);
            CHECK(check.symbols.has_value());
        }
        fs::remove_all(dir);
    });

    run("plugin inventory: a file name that is not valid Unicode refuses its plugin, not the start", [] {
        // NTFS allows a lone surrogate in a name; generic_u8string() throws on it, and
        // nothing above the inventory caught it: no plugin loaded at all.
        const std::wstring bad = L"a\xD800.txt";
        const fs::path dir = fresh("anvil-inventory-lone-surrogate");
        const fs::path anvil = dir / "anvil";
        const fs::path user = dir / "user";
        const fs::path ok = anvil / "plugins" / "ok";
        write(ok / "plugin.toml", plugin_toml("ok"));
        write(ok / "ok.dll", "MZ");
        write(ok / "translations" / bad, "x");  // read by the translation check
        const fs::path y = mod_plugin(user, "One", "y", "1.0.0");
        write(y / bad, "x");  // hashed with the mod's plugin
        enable_mods(user, {"One"});
        CHECK(fs::exists(y / bad));

        bool threw = false;
        PluginInventory inventory;
        try {
            inventory = take_inventory(anvil, user, GameImage{});
        } catch (const std::exception& e) {
            std::printf("  threw: %s\n", e.what());
            threw = true;
        }
        CHECK(!threw);
        CHECK(inventory.local.size() == 1);
        if (inventory.local.size() == 1) CHECK(inventory.local[0].check.manifest.manifest.has_value());
        CHECK(inventory.mods.size() == 1);
        if (inventory.mods.size() == 1) {
            const ModPlugin& m = inventory.mods[0].found;
            CHECK(m.hash.empty());
            CHECK(m.problem.find("not valid Unicode") != std::string::npos);
        }
        const FolderHash hash = hash_plugin_folder(y);
        CHECK(hash.sha256.empty() && hash.error.find("not valid Unicode") != std::string::npos);
        fs::remove_all(dir);
    });

    run("held folder: its hashed files cannot change, reads give the hashed bytes, a later file is missing (T3, #545)", [] {
        const fs::path dir = fresh("anvil-held-folder");
        const fs::path x = dir / "x";
        write(x / "plugin.toml", plugin_toml("x", "", "1.0.0"));
        write(x / "x.dll", "MZ");
        write(x / "Sub" / "Data.TOML", "a = 1\n");
        auto held_owner = std::make_unique<HeldFolder>(x);
        const HeldFolder& held = *held_owner;
        CHECK(held.ok() && held.hash().sha256 == hash_plugin_folder(x).sha256 && held.hash().files.size() == 3);
        // A Workshop update (or anything else) cannot write, replace, rename or delete them meanwhile.
        const HANDLE w = CreateFileW((x / "plugin.toml").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                     FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(w == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
        if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
        CHECK(!DeleteFileW((x / "x.dll").c_str()));
        CHECK(!MoveFileExW((x / "plugin.toml").c_str(), (dir / "moved.toml").c_str(), 0));
        CHECK(!MoveFileExW(x.c_str(), (dir / "x-moved").c_str(), 0));
        std::string text;
        CHECK(held.read("plugin.toml", 1 << 20, text) == HeldFolder::Read::ok && text == plugin_toml("x", "", "1.0.0"));
        CHECK(held.read("sub/data.toml", 1 << 20, text) == HeldFolder::Read::ok && text == "a = 1\n");  // as the hash names it
        CHECK(held.read("SUB\\DATA.toml", 1 << 20, text) == HeldFolder::Read::ok && text == "a = 1\n");
        CHECK(held.read("plugin.toml", 4, text) == HeldFolder::Read::too_large);
        CHECK(held.read("../x/plugin.toml", 1 << 20, text) == HeldFolder::Read::missing);
        // A file that came after the hash is not one an approval of it names.
        write(x / "settings.toml", "[setting.speed]\ntype = \"int\"\ndefault = 3\nlabel = \"Speed\"\n");
        CHECK(fs::exists(x / "settings.toml") && !held.contains("settings.toml"));
        CHECK(held.read("settings.toml", 1 << 20, text) == HeldFolder::Read::missing);
        held_owner.reset();  // let the files go
        fs::remove_all(dir);
    });

    run("plugin inventory: a mod's plugin is checked from the bytes its hash covered, not the folder later (T3, #545)", [] {
        const fs::path dir = fresh("anvil-inventory-hashed-bytes");
        const fs::path user = dir / "user";
        const fs::path x = mod_plugin(user, "One", "x", "1.0.0");
        write(x / "plugin.toml", plugin_toml("x", "settings = \"settings.toml\"\nsymbols = \"Symbols.toml\"\n", "1.0.0"));
        write(x / "symbols.toml", "builds = [\"eu4-1.37.5-steam-win64\"]\n");
        enable_mods(user, {"One"});
        // settings.toml, which plugin.toml names, comes only after the hash: an update
        // in the middle of the start. Read from the folder it would count; from the hash it cannot.
        size_t held_calls = 0;
        const ModDiscovery found = find_mod_plugins(user, [&](const ModPlugin& p, const HeldFolder& held) {
            ++held_calls;
            CHECK(held.ok() && held.hash().sha256 == p.hash);
            const HANDLE w = CreateFileW((x / "plugin.toml").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                         OPEN_EXISTING, 0, nullptr);
            CHECK(w == INVALID_HANDLE_VALUE);
            if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
            write(x / "settings.toml", "[setting.speed]\ntype = \"int\"\ndefault = 3\nlabel = \"Speed\"\n");
            const PluginCheck from_hash = check_plugin_folder(held, GameImage{});
            CHECK(!from_hash.manifest.manifest &&
                  from_hash.manifest.error.find("settings file 'settings.toml' not found") != std::string::npos);
            const PluginCheck from_disk = check_plugin_folder(x, GameImage{});
            CHECK(from_disk.manifest.manifest && from_disk.settings && from_disk.symbols);  // what the old check read
        });
        CHECK(held_calls == 1 && found.plugins.size() == 1);
        // Its plugin.toml was the hashed one, read before the late file: no manifest.
        CHECK(found.plugins.size() == 1 && !found.plugins[0].manifest && !found.plugins[0].problem.empty());
        // The plugin hashed again with settings.toml: plugin.toml, symbols (named in
        // another case) and settings, and the entry, all from the held bytes.
        auto again_owner = std::make_unique<HeldFolder>(x);
        const HeldFolder& again = *again_owner;
        const PluginCheck check = check_plugin_folder(again, GameImage{});
        CHECK(check.manifest.manifest && check.entry_missing.empty() && check.unreadable.empty());
        CHECK(check.settings && check.settings->settings.size() == 1 && check.symbols);
        // The inventory uses this check for the mod's plugin.
        const PluginInventory inventory = take_inventory(dir / "anvil", user, GameImage{});
        CHECK(inventory.mods.size() == 1 && inventory.mods[0].check.settings && inventory.mods[0].check.symbols &&
              inventory.mods[0].found.hash == again.hash().sha256);
        again_owner.reset();  // let the files go
        fs::remove_all(dir);
    });

    run("plugin inventory: an entry DLL that is not among the hashed files is missing (T3, #545)", [] {
        const fs::path dir = fresh("anvil-inventory-hashed-entry");
        const fs::path x = dir / "x";
        write(x / "plugin.toml", plugin_toml("x", "", "1.0.0"));
        auto held_owner = std::make_unique<HeldFolder>(x);
        const HeldFolder& held = *held_owner;
        write(x / "x.dll", "MZ");
        const PluginCheck check = check_plugin_folder(held, GameImage{});
        CHECK(check.manifest.manifest && check.entry_missing.code == "entry_missing" && !check.loadable());
        CHECK(check_plugin_folder(x, GameImage{}).entry_missing.empty());
        held_owner.reset();  // let the files go
        fs::remove_all(dir);
    });
}
