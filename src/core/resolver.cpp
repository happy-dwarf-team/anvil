#include "resolver.hpp"

#include "log.hpp"

#include <anvil/anvil_game.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <utility>

namespace anvil {
namespace {

std::string phase_name(Phase p) { return p == Phase::Early ? "early" : "main"; }

// Skips plugins that declare a conflict with a plugin that stays loaded. A plugin's
// conflicts are checked against the candidates that passed the earlier filters
// (a candidate that is only removed later, for its dependencies, still counts).
// The declaring plugin is the one skipped; its dependents are pruned afterwards.
//
// A plugin that is itself skipped for a conflict does not count as a target. So
// resolution starts from plugins with no live conflict targets, which are always
// kept, and skips whoever conflicts with them (A -> B -> C keeps A and C, skips B).
// When only conflict cycles are left, the smallest id is skipped and the rest is
// resolved again (A <-> B keeps B).
void prune_conflicts(std::map<std::string, Manifest>& candidates, std::vector<Skipped>& skipped) {
    // Ids (by id order) of the candidates each plugin conflicts with, with the reason text.
    std::map<std::string, std::map<std::string, std::string>> targets;
    for (const auto& [id, m] : candidates) {
        for (const auto& c : m.conflicts) {
            const auto other = candidates.find(c.id);
            if (other != candidates.end() && c.range.matches(other->second.version)) {
                targets[id].emplace(c.id, "conflicts with " + c.id + " " + other->second.version.to_string() +
                                              "; disable one of them");
            }
        }
    }
    std::set<std::string> alive;
    for (const auto& [id, m] : candidates) alive.insert(id);

    const auto live_targets = [&](const std::string& id) {
        std::vector<std::string> out;
        if (const auto it = targets.find(id); it != targets.end()) {
            for (const auto& [other, reason] : it->second) {
                if (alive.contains(other)) out.push_back(other);
            }
        }
        return out;
    };
    const auto drop = [&](const std::string& id, const std::string& other) {
        skipped.push_back({id, targets.at(id).at(other), candidates.at(id).folder});
        alive.erase(id);
    };

    for (;;) {
        // Plugins without live targets are settled: nothing can skip them any more.
        std::set<std::string> settled;
        for (const auto& id : alive) {
            if (live_targets(id).empty()) settled.insert(id);
        }
        std::vector<std::pair<std::string, std::string>> losers;  // (declaring id, settled target)
        for (const auto& id : alive) {
            for (const auto& other : live_targets(id)) {
                if (settled.contains(other)) {
                    losers.emplace_back(id, other);
                    break;
                }
            }
        }
        if (!losers.empty()) {
            for (const auto& [id, other] : losers) drop(id, other);
            continue;
        }
        // No settled target is left to decide; what remains is in conflict cycles.
        std::string first;
        for (const auto& id : alive) {
            if (!live_targets(id).empty()) {
                first = id;
                break;
            }
        }
        if (first.empty()) break;
        drop(first, live_targets(first).front());
    }

    for (auto it = candidates.begin(); it != candidates.end();) {
        it = alive.contains(it->first) ? std::next(it) : candidates.erase(it);
    }
}

// Removes plugins whose required dependencies are missing or mismatched, until stable.
void prune_dependencies(std::map<std::string, Manifest>& candidates, std::vector<Skipped>& skipped) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = candidates.begin(); it != candidates.end();) {
            const Manifest& m = it->second;
            std::string reason;
            for (const auto& d : m.dependencies) {
                const auto dep = candidates.find(d.id);
                if (dep == candidates.end()) {
                    if (d.implied) {
                        reason = "affects gameplay, so it needs plugin '" + d.id +
                                 "', which is not loaded (Anvil suspends gameplay plugins in ironman and multiplayer "
                                 "through it)";
                    } else if (!d.optional) {
                        reason = "requires plugin '" + d.id + "' (" + d.range.text() + "), which is not loaded";
                    }
                } else if (!d.range.matches(dep->second.version)) {
                    if (!d.optional) {
                        reason = "requires '" + d.id + "' " + d.range.text() + ", found " + dep->second.version.to_string();
                    }
                } else if (m.phase == Phase::Early && dep->second.phase == Phase::Main) {
                    reason = "early plugin cannot depend on main-phase plugin '" + d.id + "'";
                }
                if (!reason.empty()) break;
            }
            if (!reason.empty()) {
                skipped.push_back({m.id, reason, m.folder});
                it = candidates.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }
}

// Topological order of one phase; returns ids stuck in a cycle (empty = success).
std::vector<std::string> order_phase(const std::map<std::string, Manifest>& candidates, Phase phase,
                                     std::vector<Manifest>& order) {
    std::map<std::string, std::set<std::string>> before;  // id -> ids that must load first
    for (const auto& [id, m] : candidates) {
        if (m.phase != phase) continue;
        auto& edges = before[id];
        for (const auto& d : m.dependencies) {
            const auto dep = candidates.find(d.id);
            if (dep != candidates.end() && dep->second.phase == phase && d.range.matches(dep->second.version)) {
                edges.insert(d.id);
            }
        }
        for (const auto& a : m.load_after) {
            const auto dep = candidates.find(a);
            if (dep != candidates.end() && dep->second.phase == phase) edges.insert(a);
        }
    }
    std::set<std::string> done;
    while (!before.empty()) {
        std::vector<std::string> ready;
        for (const auto& [id, edges] : before) {
            if (std::all_of(edges.begin(), edges.end(), [&](const std::string& e) { return done.contains(e); })) {
                ready.push_back(id);
            }
        }
        if (ready.empty()) {
            std::vector<std::string> stuck;
            for (const auto& [id, edges] : before) stuck.push_back(id);
            return stuck;
        }
        std::sort(ready.begin(), ready.end());
        for (const auto& id : ready) {
            order.push_back(candidates.at(id));
            done.insert(id);
            before.erase(id);
        }
    }
    return {};
}

}  // namespace

ResolveResult resolve(const ResolveInput& input) {
    ResolveResult result;
    std::map<std::string, Manifest> candidates;
    std::set<std::string> disabled(input.disabled.begin(), input.disabled.end());

    // 1. Per-plugin checks and duplicate ids (first one wins).
    for (const auto& m : input.manifests) {
        if (candidates.contains(m.id)) {
            result.skipped.push_back(
                {m.id, "duplicate id; already provided by " + narrow(candidates[m.id].folder.wstring()), m.folder});
            continue;
        }
        if (disabled.contains(m.id)) {
            result.skipped.push_back({m.id, "disabled in anvil.toml", m.folder, true});
            continue;
        }
        if (!input.benchmark_without.empty() && m.id == input.benchmark_without) {
            Skipped s{m.id, "left out by the start-up benchmark (ANVIL_BENCHMARK=without:" + m.id + ")", m.folder, true};
            s.benchmark_arm = true;
            result.skipped.push_back(std::move(s));
            continue;
        }
        if (m.api != input.host_api) {
            result.skipped.push_back({m.id, "built for plugin API " + std::to_string(m.api) + ", Anvil provides API " +
                                                std::to_string(input.host_api), m.folder});
            continue;
        }
        if (m.anvil && input.host_version && !m.anvil->matches(*input.host_version)) {
            result.skipped.push_back({m.id, "requires Anvil " + m.anvil->text() + ", this is Anvil " +
                                                input.host_version->to_string(), m.folder});
            continue;
        }
        if (input.verify) {
            std::string reason = input.verify(m);
            if (!reason.empty()) {
                result.skipped.push_back({m.id, std::move(reason), m.folder});
                continue;
            }
        }
        Manifest& added = candidates.emplace(m.id, m).first->second;
        // Only anvil-game reports ironman and multiplayer sessions, so a
        // plugin that affects gameplay cannot run without it (design §13).
        if (m.affects_gameplay && m.id != ANVIL_GAME_ID &&
            std::none_of(m.dependencies.begin(), m.dependencies.end(),
                         [](const Dependency& d) { return d.id == ANVIL_GAME_ID; })) {
            added.dependencies.push_back({ANVIL_GAME_ID, *VersionRange::parse("*"), false, true});
        }
    }

    // 2. Declared conflicts; the declaring plugin is skipped.
    prune_conflicts(candidates, result.skipped);

    // 3. Dependencies, then topological order per phase. A cycle removes its
    // members and restarts, because other plugins may depend on them.
    for (;;) {
        prune_dependencies(candidates, result.skipped);
        result.order.clear();
        std::vector<std::string> stuck;
        Phase stuckPhase = Phase::Early;
        for (Phase phase : {Phase::Early, Phase::Main}) {
            stuck = order_phase(candidates, phase, result.order);
            if (!stuck.empty()) {
                stuckPhase = phase;
                break;
            }
        }
        if (stuck.empty()) break;
        for (const auto& id : stuck) {
            result.skipped.push_back(
                {id, "dependency/load_after cycle in " + phase_name(stuckPhase) + " phase", candidates.at(id).folder});
            candidates.erase(id);
        }
    }
    return result;
}

}  // namespace anvil
