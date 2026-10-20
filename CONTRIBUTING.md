# Contributing to Anvil

## Pull requests

Every change goes to `main` through a pull request; nobody pushes to `main`
directly. A pull request needs the `build` check (build, tests, package) and the
`pr-title` check to pass, and is **squash-merged**: the pull request title
becomes the single commit on `main` and, through it, the changelog entry. Keep
one topic per pull request; split unrelated changes.

## Commit and pull request titles

Titles follow [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<optional scope>): <summary in the imperative, lowercase, no period>
```

| Type       | Use for                                             | Release   | Changelog     |
| ---------- | --------------------------------------------------- | --------- | ------------- |
| `feat`     | new capability for players or plugin authors        | minor     | Features      |
| `fix`      | bug fix                                             | patch     | Bug Fixes     |
| `perf`     | faster or lighter, same behaviour                   | patch     | Performance   |
| `revert`   | reverting an earlier change                         | patch     | Reverts       |
| `build`    | CMake, dependencies, packaging, version resource    | patch     | Build         |
| `docs`     | documentation only                                  | none      | hidden        |
| `refactor` | code change without behaviour change                | none      | hidden        |
| `test`     | tests only                                          | none      | hidden        |
| `ci`       | GitHub Actions, Dependabot                          | none      | hidden        |
| `chore`    | anything else (release pull requests use it)        | none      | hidden        |

release-please proposes a release only when the changelog would get a visible
entry, so commits of hidden types alone never start one; they ship with the
next release.

Scopes in use: `api` (plugin SDK / `AnvilHost`), `manifest`, `core` (loader,
resolver, change registry), `proxy` (`version.dll` exports, entry hook), `log`,
`cli` (`anvil.exe`).
A scope is optional; leave it out when a change spans several.

Breaking changes: add `!` after the type or scope (`feat(api)!: …`) and a
`BREAKING CHANGE: <what breaks and how to migrate>` paragraph in the pull
request description. Before 1.0.0 a breaking change bumps the minor version.

The plugin API is a separate contract from the Anvil version:

- Within plugin API 2, host functions and fields are only **appended** after
  the "end of API 2.0" line of `anvil_plugin.h` (plugins detect them with
  `ANVIL_HOST_HAS`) and manifest keys only added. That is a
  `feat`, and plugins that need it declare `anvil = ">=x.y.z"`. Unknown
  manifest keys are errors, so an older Anvil skips a plugin that uses a newer
  key.
- Removing or changing anything a plugin can see increments
  `ANVIL_API_VERSION` and is a breaking change (`feat(api)!`).
- `sdk/rust/anvil-sys/src/bindings.rs` is generated from `anvil_plugin.h` and
  committed. After a change to the header run `sdk/rust/regen-bindings.ps1`
  (bindgen and libclang, see the script) and commit the result; CI fails when
  the file differs from what the header gives.

## Versions and releases

Releases are automated with
[release-please](https://github.com/googleapis/release-please):

1. Each push to `main` updates a pull request titled
   `chore(main): release x.y.z`. It bumps `version.txt` and adds the
   `CHANGELOG.md` section, both computed from the Conventional Commits since
   the last release.
2. Merging that pull request tags `vX.Y.Z` and creates the GitHub release with
   the changelog section as release notes.
3. The release workflow then builds the tagged commit, attaches
   `anvil-windows-x64.zip` and `SHA256SUMS.txt` with a build provenance
   attestation, publishes the `anvil-sys` crate to crates.io (only when the
   `CARGO_REGISTRY_TOKEN` repository secret exists; otherwise it warns), and asks
   the website to rebuild.

`version.txt` is the only place the version is written by hand. CMake, the
`anvil.log` line and the DLL version resource all read it, and release-please
copies it to `sdk/rust/anvil-sys/Cargo.toml`. Do not edit `version.txt` or
`CHANGELOG.md` by hand; to force a specific next version, put
`Release-As: x.y.z` in the body of a commit on `main`.

## Building and testing

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

A change that touches game memory or the loader should also be tried on a copy
of the game; say in the pull request what was and was not tested.

A change to stubs, chains or the core start-up also runs the microbenchmarks
(design §17). CI runs them and fails when a case is more than 20% slower than
`bench/baseline-ci.json` on the same runner CPU:

```powershell
build/Release/anvil_bench.exe --out bench-1.json
bench/check-regression.ps1 -Result bench-1.json
```

Binding numbers come from the lab machine: `bench/run-lab.ps1`.

A change that adds memory per change or to the core start-up also measures the
memory budgets (design §17). CI only runs this; it does not gate on the bytes:

```powershell
build/Release/anvil_bench.exe --memory --out bench-memory.json
bench/run-lab.ps1 -Memory   # exit 1 over a budget
```
