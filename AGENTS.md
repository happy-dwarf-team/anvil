# Anvil: repository rules

- Plugin loader for EU4 1.37.5 through a proxy `version.dll`; it never changes `eu4.exe` on
  disk. Design in [docs/design.md](docs/design.md), terms in [GLOSSARY.md](GLOSSARY.md).
- [CONTRIBUTING.md](CONTRIBUTING.md) holds the titles and scopes, the plugin API contract
  (append-only within API 2, regenerated Rust bindings), releases, the build and tests, and
  when the microbenchmarks and memory budgets run. Run its build and `ctest` before
  finishing a change; CI runs them.
- A change to game memory or the loader is tried on a copy of the game; the PR says what was
  and was not tested there.
- English in code and docs. Do not imply affiliation with Paradox Interactive.
- The agent owns the pull request flow: it works on a branch with Conventional Commits,
  pushes it and opens the PR as a draft. Once ready (green CI, the change tested as above,
  no blocking review findings, not put on hold by the user) it marks the PR ready and
  squash-merges it; GitHub does not allow auto-merge on a draft. Pull requests from other
  contributors get a review and a recommendation; the maintainer decides on merging them.
  Do not bypass CI or branch protection, and do not change repository settings beyond what
  auto-merge needs.
