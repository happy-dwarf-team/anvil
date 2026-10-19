// Resolver: declared conflicts (design §5.1 and §10).
#include "core/resolver.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace anvil;
using Ids = std::vector<std::string>;

Ids loaded(const ResolveResult& r) {
    Ids out;
    for (const auto& m : r.order) out.push_back(m.id);
    std::sort(out.begin(), out.end());
    return out;
}

Ids skipped_ids(const ResolveResult& r) {
    Ids out;
    for (const auto& s : r.skipped) out.push_back(s.id);
    std::sort(out.begin(), out.end());
    return out;
}

std::string reason_of(const ResolveResult& r, const std::string& id) {
    for (const auto& s : r.skipped) {
        if (s.id == id) return s.reason;
    }
    return {};
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

std::string conflict(const std::string& id, const std::string& version = "") {
    return "[[conflicts]]\nid = \"" + id + "\"\n" + (version.empty() ? "" : "version = \"" + version + "\"\n");
}

ResolveResult resolve_all(std::vector<Manifest> manifests, std::vector<std::string> disabled = {}) {
    ResolveInput in;
    in.host_api = kManifestApi;
    in.manifests = std::move(manifests);
    in.disabled = std::move(disabled);
    return resolve(in);
}

}  // namespace

void resolver_tests() {
    run("resolver conflicts: the declaring plugin is skipped with a reason", [] {
        const auto r = resolve_all({manifest(plugin_toml("a", conflict("b"))), manifest(plugin_toml("b", "", "1.2.0"))});
        CHECK((loaded(r) == Ids{"b"}));
        CHECK((skipped_ids(r) == Ids{"a"}));
        CHECK(contains(reason_of(r, "a"), "conflicts with b 1.2.0"));
        CHECK(contains(reason_of(r, "a"), "disable one of them"));
        // The result is the same for any discovery order.
        const auto flipped =
            resolve_all({manifest(plugin_toml("b", "", "1.2.0")), manifest(plugin_toml("a", conflict("b")))});
        CHECK((loaded(flipped) == Ids{"b"}) && (skipped_ids(flipped) == Ids{"a"}));
    });

    run("resolver conflicts: version ranges, default is any version", [] {
        const auto old_b = manifest(plugin_toml("b", "", "1.5.0"));
        const auto new_b = manifest(plugin_toml("b", "", "2.0.0"));
        CHECK((skipped_ids(resolve_all({manifest(plugin_toml("a", conflict("b", "<2"))), old_b})) == Ids{"a"}));
        CHECK(resolve_all({manifest(plugin_toml("a", conflict("b", "<2"))), new_b}).skipped.empty());
        CHECK((skipped_ids(resolve_all({manifest(plugin_toml("a", conflict("b"))), new_b})) == Ids{"a"}));
    });

    run("resolver conflicts: only enabled and loadable plugins count", [] {
        const auto a = manifest(plugin_toml("a", conflict("b")));
        CHECK(resolve_all({a}).skipped.empty());  // not installed
        const auto disabled = resolve_all({a, manifest(plugin_toml("b"))}, {"b"});
        CHECK((loaded(disabled) == Ids{"a"}) && (skipped_ids(disabled) == Ids{"b"}));
        auto old_api = manifest(plugin_toml("b"));
        old_api.api = 1;
        CHECK((loaded(resolve_all({a, old_api})) == Ids{"a"}));  // filtered by the host checks

        ResolveInput in;
        in.host_api = kManifestApi;
        in.manifests = {a, manifest(plugin_toml("b"))};
        in.verify = [](const Manifest& m) { return m.id == "b" ? std::string("symbols do not match") : std::string(); };
        const auto r = resolve(in);
        CHECK((loaded(r) == Ids{"a"}) && (skipped_ids(r) == Ids{"b"}));
    });

    run("resolver conflicts: the phase does not matter", [] {
        const auto r =
            resolve_all({manifest(plugin_toml("a", conflict("b"), "1.0.0", "early")), manifest(plugin_toml("b"))});
        CHECK((loaded(r) == Ids{"b"}) && (skipped_ids(r) == Ids{"a"}));
    });

    run("resolver conflicts: mutual declarations keep the larger id", [] {
        const auto r = resolve_all({manifest(plugin_toml("b", conflict("a"))), manifest(plugin_toml("a", conflict("b")))});
        CHECK((loaded(r) == Ids{"b"}));
        CHECK((skipped_ids(r) == Ids{"a"}));
        CHECK(contains(reason_of(r, "a"), "conflicts with b"));
    });

    run("resolver conflicts: a chain skips only the middle plugin", [] {
        // a -> b -> c: c is never skipped, so b is; b no longer loads, so a does not conflict with it.
        const auto r = resolve_all({manifest(plugin_toml("a", conflict("b"))), manifest(plugin_toml("b", conflict("c"))),
                                    manifest(plugin_toml("c"))});
        CHECK((loaded(r) == Ids{"a", "c"}));
        CHECK((skipped_ids(r) == Ids{"b"}));
        CHECK(contains(reason_of(r, "b"), "conflicts with c"));
        // The result does not depend on how the ids sort: c -> b -> a.
        const auto rev = resolve_all({manifest(plugin_toml("c", conflict("b"))), manifest(plugin_toml("b", conflict("a"))),
                                      manifest(plugin_toml("a"))});
        CHECK((loaded(rev) == Ids{"a", "c"}) && (skipped_ids(rev) == Ids{"b"}));
    });

    run("resolver conflicts: a cycle of three keeps the last id", [] {
        // a -> b -> c -> a: a is skipped to break the cycle, then c is settled and b conflicts with it.
        const auto r = resolve_all({manifest(plugin_toml("a", conflict("b"))), manifest(plugin_toml("b", conflict("c"))),
                                    manifest(plugin_toml("c", conflict("a")))});
        CHECK((loaded(r) == Ids{"c"}));
        CHECK((skipped_ids(r) == Ids{"a", "b"}));
    });

    run("resolver conflicts: a plugin that conflicts with two is skipped once", [] {
        const auto r = resolve_all({manifest(plugin_toml("a", conflict("b") + conflict("c"))), manifest(plugin_toml("b")),
                                    manifest(plugin_toml("c"))});
        CHECK((loaded(r) == Ids{"b", "c"}) && (skipped_ids(r) == Ids{"a"}));
        CHECK(r.skipped.size() == 1);
        CHECK(contains(reason_of(r, "a"), "conflicts with b"));
    });

    run("resolver conflicts: dependents of a skipped plugin are skipped", [] {
        const auto r = resolve_all({
            manifest(plugin_toml("a", conflict("b"))),
            manifest(plugin_toml("b")),
            manifest(plugin_toml("needs-a", "[[dependencies]]\nid = \"a\"\n")),
            manifest(plugin_toml("wants-a", "[[dependencies]]\nid = \"a\"\noptional = true\n")),
            manifest(plugin_toml("needs-needs-a", "[[dependencies]]\nid = \"needs-a\"\n")),
        });
        CHECK((loaded(r) == Ids{"b", "wants-a"}));
        CHECK((skipped_ids(r) == Ids{"a", "needs-a", "needs-needs-a"}));
        CHECK(contains(reason_of(r, "needs-a"), "requires plugin 'a'"));
    });

    run("resolver conflicts: a plugin later skipped for dependencies still counts", [] {
        // x conflicts with d, but d depends on a, which is skipped by its conflict with b.
        // Conflicts are resolved before dependencies (design §10), so d still counts and x is skipped.
        const auto r = resolve_all({
            manifest(plugin_toml("a", conflict("b"))),
            manifest(plugin_toml("b")),
            manifest(plugin_toml("d", "[[dependencies]]\nid = \"a\"\n")),
            manifest(plugin_toml("x", conflict("d"))),
        });
        CHECK((loaded(r) == Ids{"b"}));
        CHECK((skipped_ids(r) == Ids{"a", "d", "x"}));
    });
}
