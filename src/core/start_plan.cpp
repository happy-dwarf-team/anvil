#include "start_plan.hpp"

#include "crash_history.hpp"
#include "log.hpp"

#include <algorithm>
#include <functional>
#include <set>

namespace anvil {

namespace fs = std::filesystem;

namespace {

fs::path data_dir(const fs::path& anvil_dir, const std::string& id) { return anvil_dir / "data" / widen(id); }

bool listed(const std::vector<std::string>& list, const std::string& id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

// The settings.toml whose retired settings a start moves: a plugin whose
// settings.toml parses and fits its plugin.toml.
const SettingsFile* retiring(const PluginCheck& check) {
    if (!check.manifest.manifest || !check.settings_fit || check.settings->retired.empty()) return nullptr;
    return &*check.settings;
}

// The check a mod's plugin loads with: its approved copy's when that is not
// the mod's folder (an update waits), else the folder's. The copy's check is
// that of another mod's folder with its hash when one has it, never the
// waiting update's: those are not the files that load.
const PluginCheck* load_check(const PluginInventory& inventory, const ModPlugin& decided, size_t index) {
    if (decided.state == ModPluginState::UpdatePending && decided.approval) {
        return inventory.copy_check(decided.id, decided.approval->hash);
    }
    return &inventory.mods[index].check;
}

// A plugin that does not load, from what decides it.
void set_skip(PlannedPlugin& p, i18n::Message why, SkipKind kind, bool disabled) {
    p.reason = i18n::english(why);
    p.why = std::move(why);
    p.kind = kind;
    p.disabled = disabled;
    p.not_loading = disabled ? NotLoading::Off : NotLoading::CannotLoad;
}

// A candidate of the resolver: its manifest and why its check failed.
struct Candidate {
    PlannedPlugin plugin;
    Manifest manifest;
    i18n::Message check_failure;
};

}  // namespace

const char* not_loading_name(NotLoading why) {
    switch (why) {
    case NotLoading::None: return "";
    case NotLoading::Off: return "off";
    case NotLoading::NeedsPlugin: return "needs";
    case NotLoading::NotInstalled: return "not-installed";
    case NotLoading::WaitsForApproval: return "approval";
    case NotLoading::CannotLoad: return "cannot-load";
    }
    return "";
}

PlayerChoices read_player_choices(const PluginInventory& inventory) {
    return read_player_choices(inventory, read_anvil_settings(inventory.anvil_dir / "anvil.toml"));
}

PlayerChoices read_player_choices(const PluginInventory& inventory, AnvilSettings anvil_toml) {
    PlayerChoices choices;
    choices.anvil_toml = std::move(anvil_toml);
    choices.trust = read_trust(inventory.anvil_dir / "trust.toml");
    choices.moved_retired = read_moved_retired(moved_retired_file(inventory.anvil_dir));
    const auto settings_file = [&](const std::string& id) {
        if (choices.settings_files.contains(id)) return;
        std::optional<std::string> text = read_text_file(data_dir(inventory.anvil_dir, id) / "settings.toml", kMaxPluginTomlBytes);
        choices.settings_files.emplace(id, std::move(text));
    };
    for (const InventoryPlugin& p : inventory.local) {
        if (retiring(p.check) != nullptr) settings_file(p.id);
    }
    for (const InventoryModPlugin& p : inventory.mods) {
        if (retiring(p.check) != nullptr) settings_file(p.found.id);
    }
    for (const InventoryCopy& c : inventory.copies) {
        if (c.check && retiring(*c.check) != nullptr) settings_file(c.id);
    }
    return choices;
}

const PlannedPlugin* StartPlan::of_local(size_t index) const {
    for (const auto* list : {&load, &skipped}) {
        for (const PlannedPlugin& p : *list) {
            if (p.local == static_cast<int>(index)) return &p;
        }
    }
    return nullptr;
}

const PlannedPlugin* StartPlan::of_mod(size_t index) const {
    for (const auto* list : {&load, &skipped}) {
        for (const PlannedPlugin& p : *list) {
            if (p.mod == static_cast<int>(index)) return &p;
        }
    }
    return nullptr;
}

const PlannedPlugin* StartPlan::find(std::string_view id) const {
    for (const auto* list : {&load, &skipped}) {
        for (const PlannedPlugin& p : *list) {
            if (p.id == id) return &p;
        }
    }
    return nullptr;
}

std::vector<RetiredMoveNote> carry_out_retired_move_now(const PlannedRetiredMove& move, const fs::path& anvil_toml,
                                                        bool* anvil_toml_written, bool* settled) {
    AnvilSettings lists = read_anvil_settings(anvil_toml);
    return move_retired_settings(move.manifest, move.schema, move.player_file, anvil_toml, lists, true, anvil_toml_written,
                                 moved_retired_file(anvil_toml.parent_path()), settled);
}

const PluginCheck* check_of(const PluginInventory& inventory, const StartPlan& plan, const PlannedPlugin& plugin) {
    if (plugin.local >= 0) return &inventory.local[static_cast<size_t>(plugin.local)].check;
    if (plugin.mod >= 0) {
        const size_t i = static_cast<size_t>(plugin.mod);
        return load_check(inventory, plan.mods[i], i);
    }
    return nullptr;
}

StartPlan plan_start(const PluginInventory& inventory, const PlayerChoices& choices, const StartConditions& conditions) {
    StartPlan plan;
    // A broken anvil.toml loads nothing (K3, #537): its lists are unknown.
    const bool broken = !choices.anvil_toml.error.empty();
    plan.enabled = choices.anvil_toml.enabled && !broken && !conditions.anvil_disabled;
    plan.safe_mode = plan.enabled && conditions.safe_mode;
    plan.lists = choices.anvil_toml;
    const bool writes = plan.enabled;  // a start that loads nothing writes nothing

    // The playset's plugins, decided against trust.toml (design §14).
    for (const InventoryModPlugin& m : inventory.mods) plan.mods.push_back(m.found);
    decide_mod_plugins(plan.mods, choices.trust, inventory.local_ids(), inventory.anvil_dir,
                       [&](const std::string& id, const std::string& hash) { return inventory.copy_exists(id, hash); });
    if (const std::string warning = trust_warning(choices.trust); !warning.empty()) plan.warnings.push_back(warning);

    // The approved copies trust.toml no longer names.
    if (writes && choices.trust.error.empty()) {
        for (const fs::path& id_dir : inventory.copy_ids) {
            const std::string id = narrow(id_dir.filename().wstring());
            const TrustEntry* e = choices.trust.find(id);
            if (e == nullptr) {
                plan.writes.remove.push_back(id_dir);
                continue;
            }
            for (const InventoryCopy& c : inventory.copies) {
                if (c.id == id && c.name != e->hash) plan.writes.remove.push_back(c.folder);
            }
        }
    }

    // Discovery: what skips a plugin before the player's choices count.
    std::vector<Candidate> candidates;
    for (size_t i = 0; i < inventory.local.size(); ++i) {
        const InventoryPlugin& local = inventory.local[i];
        PlannedPlugin p;
        p.id = local.id;
        p.folder = local.folder;
        p.local = static_cast<int>(i);
        const auto& parsed = local.check.manifest.manifest;
        if (parsed) {
            p.manifest = *parsed;
            add_implied_dependencies(*p.manifest);
        }
        const bool off = !plan.enabled || listed(plan.lists.disabled_plugins, p.id);
        if (!parsed) {
            set_skip(p, local.check.skip, SkipKind::other, !plan.enabled);
            p.not_loading = NotLoading::CannotLoad;  // nothing the player can switch on
        } else if (!local.check.entry_missing.empty()) {
            set_skip(p, local.check.entry_missing, SkipKind::other, off);
        } else if (plan.safe_mode && !loads_in_safe_mode(p.id)) {
            set_skip(p, i18n::Message("safe_mode", {}), SkipKind::safe_mode, false);
        } else if (i18n::Message why = plan.safe_mode ? safe_mode_exclusion(p.id, conditions.crashes) : i18n::Message();
                   !why.empty()) {
            set_skip(p, std::move(why), SkipKind::safe_mode, false);
        } else {
            candidates.push_back({p, *parsed, local.check.skip});
            continue;
        }
        plan.skipped.push_back(std::move(p));
    }
    for (size_t i = 0; i < plan.mods.size(); ++i) {
        const ModPlugin& m = plan.mods[i];
        if (m.state == ModPluginState::Shadowed) continue;
        PlannedPlugin p;
        p.id = m.id;
        p.folder = m.folder;
        p.mod = static_cast<int>(i);
        p.mod_name = m.mod.name;
        const i18n::Message source =
            m.mod.workshop_id.empty() ? i18n::Message("local_mod", {}) : i18n::Message("workshop_item", {m.mod.workshop_id});
        const auto skip = [&](const i18n::Message& reason, SkipKind kind) {
            set_skip(p, i18n::Message("from_mod", {m.mod.name, source, reason}), kind, false);
            p.awaiting_approval = !m.loads() && (m.state == ModPluginState::Pending || m.state == ModPluginState::Conflict);
            plan.skipped.push_back(p);
        };
        // No check of the copy that would load: trust.toml changed after the
        // inventory was taken (the next start's plan during a game). Nothing
        // loads on a check of other files.
        static const PluginCheck unchecked = [] {
            PluginCheck c;
            c.skip = i18n::Message("the approved copy was not checked; restart the game");
            return c;
        }();
        const PluginCheck* checked = load_check(inventory, m, i);
        const PluginCheck& check = checked != nullptr ? *checked : unchecked;
        if (check.manifest.manifest) {
            p.manifest = *check.manifest.manifest;
            add_implied_dependencies(*p.manifest);
        } else if (m.manifest) {
            p.manifest = *m.manifest;
            add_implied_dependencies(*p.manifest);
        }
        if (!m.loads()) {
            skip(m.why, m.state == ModPluginState::Pending    ? SkipKind::approval
                        : m.state == ModPluginState::Conflict ? SkipKind::conflict
                                                               : SkipKind::other);
            if (m.state == ModPluginState::Pending) plan.skipped.back().not_loading = NotLoading::WaitsForApproval;
            continue;
        }
        if (plan.safe_mode) {
            skip(i18n::Message("safe_mode", {}), SkipKind::safe_mode);
            continue;
        }
        p.folder = m.load_folder;
        if (p.manifest) p.manifest->folder = m.load_folder;
        p.approved_hash = m.approval->hash;
        if (m.copy_missing && writes) {
            p.makes_copy = true;
            plan.writes.copies.push_back({m.id, static_cast<int>(i), m.folder, m.load_folder, m.approval->hash});
        }
        if (!check.manifest.manifest) {
            skip(i18n::Message("copy_broken", {check.skip}), SkipKind::other);
            continue;
        }
        if (!check.entry_missing.empty()) {
            skip(i18n::Message("copy_broken", {check.entry_missing}), SkipKind::other);
            continue;
        }
        candidates.push_back({p, *p.manifest, check.skip});
    }

    // The resolver: the player's switch, the arm, the host, the check, conflicts, dependencies.
    std::map<fs::path, size_t> by_folder;
    for (size_t i = 0; i < candidates.size(); ++i) by_folder.emplace(candidates[i].manifest.folder, i);
    if (!plan.enabled) {
        for (Candidate& c : candidates) {
            set_skip(c.plugin,
                     broken ? i18n::Message("anvil_toml_broken", {std::to_string(choices.anvil_toml.line)})
                            : i18n::Message("all_disabled", {}),
                     SkipKind::other, true);
            plan.skipped.push_back(std::move(c.plugin));
        }
    } else {
        ResolveInput input;
        input.host_api = conditions.host_api;
        input.host_version = conditions.host_version;
        input.disabled = plan.lists.disabled_plugins;
        if (conditions.benchmark && !conditions.benchmark->with) input.benchmark_without = conditions.benchmark->plugin;
        for (const Candidate& c : candidates) {
            input.manifests.push_back(c.manifest);
            input.check_failures.push_back(c.check_failure);
        }
        ResolveResult resolved = resolve(input);
        for (Manifest& m : resolved.order) {
            PlannedPlugin p = candidates[by_folder.at(m.folder)].plugin;
            p.manifest = std::move(m);
            plan.load.push_back(std::move(p));
        }
        for (Skipped& s : resolved.skipped) {
            const auto it = by_folder.find(s.folder);
            PlannedPlugin p = it != by_folder.end() ? candidates[it->second].plugin : PlannedPlugin{};
            if (it == by_folder.end()) {
                p.id = s.id;
                p.folder = s.folder;
            }
            p.reason = std::move(s.reason);
            p.why = std::move(s.why);
            p.kind = s.kind;
            p.disabled = s.disabled;
            p.benchmark_arm = s.benchmark_arm;
            p.not_loading = s.disabled ? NotLoading::Off : NotLoading::CannotLoad;
            plan.skipped.push_back(std::move(p));
        }
    }

    // Why a plugin skipped for a dependency does not load, as the player
    // reads it: the first required plugin that does not load, and the path to
    // the one that is missing (the Plugins tab's line, ui-guidelines §5).
    std::set<std::string> loading;
    for (const PlannedPlugin& p : plan.load) loading.insert(p.id);
    std::function<void(PlannedPlugin&, std::vector<std::string>&)> classify = [&](PlannedPlugin& p,
                                                                                   std::vector<std::string>& visiting) {
        if (p.why.code != "requires_missing" && p.why.code != "needs_anvil_game") return;
        p.not_loading = NotLoading::CannotLoad;
        if (!p.manifest) return;
        visiting.push_back(p.id);
        for (const Dependency& d : p.manifest->dependencies) {
            if (d.optional || loading.contains(d.id)) continue;
            if (std::find(visiting.begin(), visiting.end(), d.id) != visiting.end()) continue;
            PlannedPlugin* dep = nullptr;
            for (PlannedPlugin& other : plan.skipped) {
                if (other.id == d.id) {
                    dep = &other;
                    break;
                }
            }
            if (dep == nullptr) {
                if (inventory.installed(d.id)) continue;  // shadowed: the local plugin decides
                p.not_loading = NotLoading::NotInstalled;
                p.missing = d.id;
                break;
            }
            classify(*dep, visiting);
            if (dep->not_loading == NotLoading::NeedsPlugin || dep->not_loading == NotLoading::NotInstalled) {
                p.not_loading = dep->not_loading;
                p.missing = dep->missing;
                p.through = {d.id};
                p.through.insert(p.through.end(), dep->through.begin(), dep->through.end());
            } else {
                p.not_loading = NotLoading::NeedsPlugin;
                p.missing = d.id;
            }
            break;
        }
        visiting.pop_back();
    };
    for (PlannedPlugin& p : plan.skipped) {
        if (!p.disabled && p.not_loading == NotLoading::CannotLoad && p.missing.empty()) {
            std::vector<std::string> visiting;
            classify(p, visiting);
        }
    }

    // Retired settings move into anvil.toml before the modules are resolved,
    // so the player's choice holds at this start (ADR 0013, point 4): of
    // every local plugin whose settings.toml fits, loading or not, and of
    // the mods' plugins that load.
    if (writes) {
        std::set<std::string> moved;
        const auto move = [&](const std::string& id, const Manifest& manifest, const SettingsFile& schema) {
            if (!moved.insert(id).second) return;
            const auto text = choices.settings_files.find(id);
            const std::optional<std::string> player_text = text != choices.settings_files.end() ? text->second : std::nullopt;
            // A benchmark start writes no file (docs/spec/benchmark-v1.md §2): the
            // values hold for its lists only and stay for the next start. A value the
            // record of moved retired settings holds is only removed from the player's file.
            const auto recorded = choices.moved_retired.find(id);
            RetiredMovePlan m = plan_retired_settings(manifest, schema, player_text, plan.lists, !conditions.benchmark,
                                                      recorded != choices.moved_retired.end() ? recorded->second
                                                                                              : std::vector<MovedRetired>{});
            if (!m.writes()) {
                if (m.forget_recorded) plan.writes.forget_moved.push_back(id);
                for (RetiredMoveNote& note : m.notes) plan.notes.emplace_back(id, std::move(note));
                return;
            }
            plan.writes.retired.push_back({id, data_dir(inventory.anvil_dir, id) / "settings.toml", std::move(m), manifest, schema});
        };
        for (const InventoryPlugin& local : inventory.local) {
            // Not a second folder with the id of another: its data/<id> is the other's.
            const bool duplicate = std::any_of(plan.skipped.begin(), plan.skipped.end(), [&](const PlannedPlugin& s) {
                return s.mod < 0 && s.folder == local.folder && s.why.code == "duplicate_id";
            });
            if (duplicate) continue;
            // Of an id that loads, only the folder that loads: a first folder that fails another
            // check (a newer Anvil) leaves the loaded one's data/<id> alone.
            const bool other_loads = std::any_of(plan.load.begin(), plan.load.end(), [&](const PlannedPlugin& l) {
                return l.mod < 0 && l.id == local.id && l.folder != local.folder;
            });
            if (other_loads) continue;
            if (const SettingsFile* schema = retiring(local.check)) move(local.id, *local.check.manifest.manifest, *schema);
        }
        for (const PlannedPlugin& p : plan.load) {
            if (p.mod < 0) continue;
            const PluginCheck* check = check_of(inventory, plan, p);
            if (const SettingsFile* schema = check != nullptr ? retiring(*check) : nullptr) move(p.id, *p.manifest, *schema);
        }
    }

    // Every module on or off at this start (anvil.toml, `default`; ADR 0011).
    for (auto* list : {&plan.load, &plan.skipped}) {
        for (PlannedPlugin& p : *list) {
            if (!p.manifest) continue;
            for (const Module& mod : p.manifest->modules) {
                p.modules_on.push_back(module_enabled(plan.lists, module_key(*p.manifest, mod), mod.enabled_by_default));
            }
        }
    }
    return plan;
}

NextStart read_next_start(const fs::path& anvil_dir) {
    NextStart next;
    next.record = read_start_record(anvil_dir / "data" / "anvil" / "unfinished-starts.txt");
    next.conditions.safe_mode = next.record.safe_mode;
    if (next.record.safe_mode) next.conditions.crashes = safe_mode_crashes(anvil_dir / "crashes", next.record.unfinished);
    return next;
}

}  // namespace anvil
