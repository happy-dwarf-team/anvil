#include "resolver.hpp"

#include "log.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace anvil {
namespace {

std::string phase_name(Phase p) { return p == Phase::Early ? "early" : "main"; }

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
                    if (!d.optional) reason = "requires plugin '" + d.id + "' (" + d.range.text() + "), which is not loaded";
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
                skipped.push_back({m.id, reason});
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
            result.skipped.push_back({m.id, "duplicate id; already provided by " + narrow(candidates[m.id].folder.wstring())});
            continue;
        }
        if (disabled.contains(m.id)) {
            result.skipped.push_back({m.id, "disabled in anvil.toml"});
            continue;
        }
        if (m.api != input.host_api) {
            result.skipped.push_back({m.id, "built for plugin API " + std::to_string(m.api) + ", Anvil provides API " +
                                                std::to_string(input.host_api)});
            continue;
        }
        if (m.anvil && input.host_version && !m.anvil->matches(*input.host_version)) {
            result.skipped.push_back({m.id, "requires Anvil " + m.anvil->text() + ", this is Anvil " +
                                                input.host_version->to_string()});
            continue;
        }
        if (input.verify) {
            std::string reason = input.verify(m);
            if (!reason.empty()) {
                result.skipped.push_back({m.id, std::move(reason)});
                continue;
            }
        }
        candidates.emplace(m.id, m);
    }

    // 2./3. Dependencies, then topological order per phase. A cycle removes its
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
            result.skipped.push_back({id, "dependency/load_after cycle in " + phase_name(stuckPhase) + " phase"});
            candidates.erase(id);
        }
    }
    return result;
}

}  // namespace anvil
