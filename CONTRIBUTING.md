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
resolver, patch registry), `proxy` (`version.dll` exports, entry hook), `log`.
A scope is optional; leave it out when a change spans several.

Breaking changes: add `!` after the type or scope (`feat(api)!: …`) and a
`BREAKING CHANGE: <what breaks and how to migrate>` paragraph in the pull
request description. Before 1.0.0 a breaking change bumps the minor version.

The plugin API is a separate contract from the Anvil version:

- Within plugin API 1, host functions and fields are only **appended** (plugins
  detect them with `ANVIL_HOST_HAS`) and manifest keys only added. That is a
  `feat`, and plugins that need it declare `anvil = ">=x.y.z"`.
- Removing or changing anything a plugin can see increments
  `ANVIL_API_VERSION` and is a breaking change (`feat(api)!`).

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
   attestation, and asks the website to rebuild.

`version.txt` is the only place the version is written. CMake, the `anvil.log`
line and the DLL version resource all read it. Do not edit `version.txt` or
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
