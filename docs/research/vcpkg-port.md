# A vcpkg port of the Anvil SDK

Research note for the fog item "a vcpkg port" in [`docs/design.md`](../design.md)
section 21, which came from decision 1 of
[issue #21](https://github.com/happy-dwarf-team/anvil/issues/21) ("A vcpkg port moves to
the map's fog") and the open item "A vcpkg port of the SDK, if authors ask for one" in
[issue #12](https://github.com/happy-dwarf-team/anvil/issues/12). Researched 2026-10-26
against the vcpkg documentation on learn.microsoft.com, the `microsoft/vcpkg`
repository and its pull requests, CMake's documentation, and the Anvil tree at
`d2b7ca4` (read only). No port was written, built or submitted; nothing below has been
tried with vcpkg.

Sections: **verified facts** (each with its source), **inferences** (reasoning from
the facts, not observed), **options for Anvil**, and **open questions**.

## Question

Should the Anvil SDK (`anvil-sdk-windows-x64.zip`: headers, `bin/anvil.exe`, the
`AnvilPlugin` CMake package) also be offered through vcpkg? Specifically:

1. What does the curated registry (`microsoft/vcpkg`) require of a new port, and does
   Anvil meet it?
2. What do the alternatives cost: a custom Git registry owned by `happy-dwarf-team`,
   an overlay port, or only documenting `FetchContent`?
3. Would a port give plugin authors anything they do not already get from
   `FetchContent` or `find_package(AnvilPlugin)`, given that `anvil.exe` must run on the
   build host?

## Answer

- **The curated registry is closed to Anvil until about mid-April 2027.** New ports must
  be "mature": a release at least six months old or six months of public development.
  The repository was created on 2026-10-14 and the first release (v0.2.0) is from
  2026-10-15. Maintainers close new-port PRs that miss this bar, routinely, within days.
- **The name `anvil` would be refused as ambiguous.** The port would have to be
  `happy-dwarf-team-anvil` (the guide's `<github owner>-<repository name>` form).
- **A curated port must build `anvil.exe` from source, and the tree cannot do that today
  without patches.** The root `CMakeLists.txt` always builds `version.dll`, both standard
  plugins and AnvilManager, fetches toml++, SafetyHook (+ Zydis, Zycore), Dear ImGui and
  miniz with `FetchContent` from Git (nothing is vendored), forces
  `SAFETYHOOK_FETCH_ZYDIS` with `FORCE`, links the non-namespaced `Zydis` target, and
  installs the SDK in an archive layout (`cmake/`, `bin/`, files at the prefix root)
  rather than vcpkg's (`share/<package>/`, `tools/<port>/`). vcpkg builds ports with
  `FETCHCONTENT_FULLY_DISCONNECTED=ON`, so the `FetchContent` calls cannot download.
  All of anvil.exe's own dependencies (toml++ 3.4.0, Zydis 4.1.1, Zycore) are already
  curated ports.
- **A custom Git registry is the only vcpkg route available now.** It has no maturity,
  naming or review gate and may even repackage the prebuilt SDK zip. The SKSE world
  did this (CommonLibSSE-NG through the Color-Glass registry on GitLab), but that
  registry has had no activity since 2024-05, and the current CommonLibSSE uses xmake.
  Each Anvil release (8 in 12 days so far) would need a new port version plus
  `x-add-version` in the registry. The release workflow could do it, but only with a
  credential for a second repository, because `GITHUB_TOKEN` is limited to the
  workflow's own repository.
- **For authors the gain is small.** The SDK is header-only plus a host tool, so vcpkg
  adds no link-time value. It helps only authors who already manage other dependencies
  with vcpkg and want one manifest. They can already mix: `FetchContent` of the SDK works
  in a project that uses the vcpkg toolchain. A from-source port would make every author
  compile toml++, Zydis and anvil.exe once per host triplet (binary caching aside).
  It would also give up the archive's build attestation and the "tied to the release"
  guarantee, unless the port pins the same tag.
- **Recommendation (inference):** keep `FetchContent`/`find_package` as the supported
  path. Keep the item in the fog, now with a date: a curated port can first be proposed
  around 2027-04-15, and only if authors ask. If someone asks earlier, publish an
  overlay port or a small `happy-dwarf-team/vcpkg-registry` that wraps the released zip.
  Do not restructure the build for vcpkg before then.

## What building anvil.exe from source needs (Anvil tree at `d2b7ca4`)

Verified by reading the files; nothing was built.

1. `anvil.exe` is `add_executable(anvil src/cli/anvil_cli.cpp)` linked to `anvil_tool`
   → `anvil_core`, which links `tomlplusplus::tomlplusplus Zydis bcrypt`
   (`CMakeLists.txt` lines 60, 104). It does **not** use SafetyHook, Dear ImGui or
   miniz. Those belong to `anvil_loader`/`version.dll`, `anvil-overlay` and the Manager.
2. Dependencies come from `FetchContent_Declare(... GIT_REPOSITORY ... GIT_TAG ...)`, not
   from vendored copies: toml++ `v3.4.0` and SafetyHook `v0.7.0` (`CMakeLists.txt`
   lines 24–32), Dear ImGui `v1.91.8` (`plugins/anvil-overlay/CMakeLists.txt` line 16),
   miniz `3.0.2` (`tools/manager/CMakeLists.txt` line 11). Zydis comes in through
   SafetyHook's own fetch: line 28 sets `SAFETYHOOK_FETCH_ZYDIS ON CACHE BOOL "" FORCE`.
   SafetyHook v0.7.0 fetches Zydis `v4.1.0`
   ([cmake.toml at v0.7.0](https://github.com/cursey/safetyhook/blob/v0.7.0/cmake.toml)).
   None of the declarations has `FIND_PACKAGE_ARGS`.
3. There is no "SDK only" switch. `ANVIL_BUILD_TESTS`, `ANVIL_BUILD_EXAMPLES` and
   `ANVIL_BUILD_BENCH` default to `ON` (lines 19–21). `plugins/anvil-game`,
   `plugins/anvil-overlay` and `tools/manager` are always added (lines 166, 169, 183),
   the standard plugins with `CMAKE_COMPILE_WARNING_AS_ERROR ON`.
4. The static CRT is forced for the whole tree with `CMAKE_MSVC_RUNTIME_LIBRARY`
   (line 17), whatever triplet a caller asks for.
5. The SDK install component puts `include/anvil/*`, `bin/anvil.exe`,
   `cmake/AnvilPluginConfig{,Version}.cmake`, and at the prefix root `CMakeLists.txt`,
   `README.md`, `version.txt` and `LICENSE`. The third-party licenses are read from
   `${tomlplusplus_SOURCE_DIR}` and `${zydis_SOURCE_DIR}` (lines 403–409), which exist
   only when `FetchContent` populated them.
6. `AnvilPluginConfig.cmake` finds everything relative to itself:
   `_anvil_sdk_dir = ${CMAKE_CURRENT_LIST_DIR}/..`, then `include/` and `bin/anvil.exe`
   under it (lines 29–39). It already honours `ANVIL_SDK_INCLUDE_DIR` and
   `ANVIL_SDK_TOOL` when they are set beforehand, and it calls no `find_dependency`.
7. The installed headers include only the standard library and each other (`<format>`,
   `<span>`, `<filesystem>`, …; checked with `grep "#include" include/anvil/*`).

## Verified facts

### Curated registry policy

1. **Maturity.** "Components packaged in the curated registry must be mature". One of:
   a release at least six months old, six months of active public development, an
   official part of such a project, or equivalent maturity. "Components intended for
   personal use should be published to custom registries"
   ([maintainer guide, "Packaged components should be mature"](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#packaged-components-should-be-mature)).
   The new-port checklist in the PR template asks for exactly this
   ([pull_request_template.md](https://github.com/microsoft/vcpkg/blob/master/.github/pull_request_template.md)).
2. **Anvil's age.** `happy-dwarf-team/anvil` was created 2026-10-14T23:37Z. Its first
   release, v0.2.0, was published 2026-10-15, and v1.5.0 on 2026-10-26: 8 releases
   (`gh repo view`, `gh release list -R happy-dwarf-team/anvil`). The repository is
   public and MIT-licensed, with 0 stars and 0 forks today.
3. **Active maintenance.** Ports of inactive components may be delisted
   ([maintainer guide](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#packaged-components-should-be-stable-and-actively-maintained)).
4. **Prebuilt binaries** are "allowed but strongly discouraged". A port is rejected only
   if it installs prebuilt binaries, those incorporate dependencies provided by other
   curated ports, *and* the artifacts expose them to a user's link domain. Explicitly
   allowed (still discouraged): "Host-only helper tools used at build time whose
   outputs are consumed but which aren't linked against by dependent ports, provided
   that they isolate bundled dependencies"
   ([maintainer guide, "Installing prebuilt binaries"](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#installing-prebuilt-binaries)).
5. **Vendored dependencies:** "devendor dependencies whenever practical". A vendored copy
   is allowed only when fully isolated, e.g. "a host-only build tool can contain a
   statically linked dependency"
   ([maintainer guide, "Do not use vendored dependencies"](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#do-not-use-vendored-dependencies)).
6. **No downloads in a port build:** `vcpkg_cmake_configure` prepends
   `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`
   ([vcpkg_cmake_configure.cmake, line 218](https://github.com/microsoft/vcpkg/blob/master/ports/vcpkg-cmake/vcpkg_cmake_configure.cmake)).
   CMake: with it on, "no attempt is made to download or update any content", and
   since 3.30 (CMP0170) the requirement that content is already populated is enforced.
   Since 3.24, `FIND_PACKAGE_ARGS` or `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=ALWAYS` lets
   `FetchContent_MakeAvailable` try `find_package()` first
   ([CMake FetchContent](https://cmake.org/cmake/help/latest/module/FetchContent.html)).
7. **Cache variables set with `FORCE`** cannot be overridden from the command line
   ([maintainer guide, "Prefer options over patching"](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#prefer-options-over-patching)).
8. **Tests and examples** must be off by default
   ([maintainer guide](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#do-not-build-testsdocsexamples-by-default)).
   Patches that upstream might want need upstream notification and a 30-day wait,
   except for high-confidence ones such as "Disabling irrelevant-in-vcpkg components"
   ([maintainer guide, "Upstream notification and waiting periods"](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#upstream-notification-and-waiting-periods)).
   When AI code review flags a bug in a new port, it must be reported upstream, and
   with no response in 30 days the submission is closed (same section).
9. **License and copyright:** `"license"` is an SPDX expression
   ([vcpkg.json reference](https://learn.microsoft.com/en-us/vcpkg/reference/vcpkg-json#license)).
   Each port installs `share/${PORT}/copyright`, preferably with
   `vcpkg_install_copyright(FILE_LIST ...)`
   ([maintainer guide, "Install copyright file"](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#install-copyright-file)).
10. **CMake configs:** a config that upstream does not ship must be named
    `unofficial-<port>`
    ([maintainer guide](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#add-cmake-exports-in-an-unofficial--namespace)).
    `AnvilPlugin` is upstream's own package, so it keeps its name. Configs go to
    `share/<lowercase-package>/<package>Config.cmake` (for `find_package(AnvilPlugin)`:
    `share/anvilplugin/AnvilPluginConfig.cmake`). Executables go to `tools/<port>`.
    `copyright`/`usage` go to `share/<port>`
    ([installation tree layout](https://learn.microsoft.com/en-us/vcpkg/reference/installation-tree-layout)).
    `vcpkg_cmake_config_fixup` takes `PACKAGE_NAME` when it differs from the port name and
    rewrites `/bin/*.exe` references to `TOOLS_PATH`
    ([vcpkg_cmake_config_fixup](https://learn.microsoft.com/en-us/vcpkg/maintainers/functions/vcpkg_cmake_config_fixup)).
    `vcpkg_copy_tools` copies to `tools/${PORT}` by default
    ([vcpkg_copy_tools](https://learn.microsoft.com/en-us/vcpkg/maintainers/functions/vcpkg_copy_tools)).
11. **Sources:** `vcpkg_from_github(REPO … REF <tag> SHA512 …)`. The SHA512 is that of
    `https://github.com/${REPO}/archive/${REF}.tar.gz`
    ([vcpkg_from_github](https://learn.microsoft.com/en-us/vcpkg/maintainers/functions/vcpkg_from_github)).
    `vcpkg_download_distfile` also requires a SHA512 (SHA-256 is not accepted)
    ([vcpkg_download_distfile](https://learn.microsoft.com/en-us/vcpkg/maintainers/functions/vcpkg_download_distfile)).
    Anvil's release publishes SHA-256 sums only (`SHA256SUMS.txt`, `release.yml`).

### Name

12. "Ports with short names or named after common words require disambiguation". An
    accepted prefix is "`<github owner>-<repository name>`"
    ([maintainer guide, "Use distinctive port names"](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#use-distinctive-port-names)).
    The checklist asks for Repology, a top web result, or the `Owner-Project` form
    ([PR template](https://github.com/microsoft/vcpkg/blob/master/.github/pull_request_template.md)).
13. There is no port named `anvil*`, `happy*` or `eu4*` among the 2,887 entries of
    `ports/` on `master` (`gh api repos/microsoft/vcpkg/git/trees/<ports sha>`).
    Repology's `anvil` project already maps to three unrelated packages: an AUR Linux
    kernel tool, `anvil-bin` and FreeBSD `sysutils/anvil`
    ([Repology API](https://repology.org/api/v1/project/anvil)).

### Triplets, CRT and `supports`

14. Every curated port must be tested in CI on at least one official triplet
    ([maintainer guide](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#ports-must-be-tested-in-at-least-one-official-triplet)).
    The official (non-community) triplets include `x64-windows`, `x64-windows-static`,
    `x64-windows-static-md` and `x64-windows-release`
    ([triplets/](https://github.com/microsoft/vcpkg/tree/master/triplets)).
    `x64-windows-static` sets `VCPKG_CRT_LINKAGE static` and `VCPKG_LIBRARY_LINKAGE static`
    ([x64-windows-static.cmake](https://github.com/microsoft/vcpkg/blob/master/triplets/x64-windows-static.cmake)).
    The default triplet on Windows is `x64-windows`
    ([triplets concept](https://learn.microsoft.com/en-us/vcpkg/concepts/triplets)).
15. `supports` is a platform expression with identifiers `x64`, `windows`, `uwp`,
    `mingw`, `xbox`, `static`, `staticcrt`, `native`
    ([vcpkg.json reference, Platform Expression](https://learn.microsoft.com/en-us/vcpkg/reference/vcpkg-json#platform-expression)).
    Windows-only hooking ports use it: `minhook` `"windows & !uwp & !arm"`, `safetyhook`
    `"(windows | linux) & !arm & !uwp"`, `detours` `"windows & !uwp"`
    ([minhook](https://github.com/microsoft/vcpkg/blob/master/ports/minhook/vcpkg.json),
    [safetyhook](https://github.com/microsoft/vcpkg/blob/master/ports/safetyhook/vcpkg.json),
    [detours](https://github.com/microsoft/vcpkg/blob/master/ports/detours/vcpkg.json)).

### Host tools

16. A tool consumed at build time is a dependency with `"host": true`. It is built for
    the host triplet (default `x64-windows` on x64 Windows), found in a portfile under
    `CURRENT_HOST_INSTALLED_DIR/tools/<port>`. A top-level CMake project gets host
    tools on `CMAKE_PROGRAM_PATH` with `VCPKG_USE_HOST_TOOLS`
    ([host dependencies](https://learn.microsoft.com/en-us/vcpkg/users/host-dependencies)).
    The `host` field is documented for any dependency, without a restriction to ports
    ([vcpkg.json reference, Dependency host](https://learn.microsoft.com/en-us/vcpkg/reference/vcpkg-json#dependency-host)).
17. `VCPKG_CROSSCOMPILING` is true whenever `TARGET_TRIPLET != HOST_TRIPLET`, "ports
    must not assume that they can run the results of a build"
    ([portfile variables](https://learn.microsoft.com/en-us/vcpkg/maintainers/variables)).
    So a plugin built for `x64-windows-static` with host `x64-windows` counts as
    cross-compiling.
18. The curated pattern for a library that ships its own generator is a host dependency
    on itself. `protobuf` and `flatbuffers` list `{"name": "<self>", "host": true}`
    ([protobuf](https://github.com/microsoft/vcpkg/blob/master/ports/protobuf/vcpkg.json),
    [flatbuffers](https://github.com/microsoft/vcpkg/blob/master/ports/flatbuffers/vcpkg.json)).
    protobuf builds `protoc` only when `TARGET_TRIPLET == HOST_TRIPLET` and otherwise
    copies `CURRENT_HOST_INSTALLED_DIR/tools/${PORT}` into the target package
    ([protobuf portfile](https://github.com/microsoft/vcpkg/blob/master/ports/protobuf/portfile.cmake)).
    flatbuffers includes the host triplet's `FlatcTargets.cmake` from its config
    ([flatbuffers portfile](https://github.com/microsoft/vcpkg/blob/master/ports/flatbuffers/portfile.cmake)).

### anvil.exe's dependencies in the curated registry

19. `tomlplusplus` 3.4.0 (port-version 1, MIT, built with Meson as a compiled library),
    `zydis` 4.1.1 (port-version 1, MIT, depends on `zycore`), `zycore` 1.5.2,
    `safetyhook` 0.7.0 (BSL-1.0, configured with `SAFETYHOOK_FETCH_ZYDIS=OFF`), `imgui`
    1.93.0 and `miniz` 3.1.2 are all curated ports
    (`ports/<name>/vcpkg.json` and `portfile.cmake` on `master`, e.g.
    [zydis](https://github.com/microsoft/vcpkg/blob/master/ports/zydis/portfile.cmake),
    [tomlplusplus](https://github.com/microsoft/vcpkg/blob/master/ports/tomlplusplus/portfile.cmake),
    [safetyhook](https://github.com/microsoft/vcpkg/blob/master/ports/safetyhook/portfile.cmake)).
    `minhook`, `polyhook2` and `detours` are also there. `kiero` and any `commonlib*` are not.
20. Zydis's installed config exports `NAMESPACE "Zydis::"`
    ([zydis CMakeLists.txt v4.1.1, line 297](https://github.com/zyantific/zydis/blob/v4.1.1/CMakeLists.txt)).
    Anvil links the bare target `Zydis`, which only the `FetchContent` build defines.
    toml++ defines `tomlplusplus::tomlplusplus` both ways
    ([tomlplusplus CMakeLists.txt v3.4.0, line 17](https://github.com/marzer/tomlplusplus/blob/v3.4.0/CMakeLists.txt)).

### Versions and registries

21. Each change to a port bumps `version` or `port-version`. `versions/baseline.json` and
    `versions/<letter>-/<port>.json` are updated with `vcpkg x-add-version`. PRs are
    squash-merged and should touch one port
    ([maintainer guide, Versioning and PR structure](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#versioning)).
22. A top-level project gets the version in its registry's baseline. Newer versions need
    a baseline update (`vcpkg x-update-baseline`) or a `version>=`. vcpkg picks the
    minimum version satisfying all constraints
    ([versioning reference](https://learn.microsoft.com/en-us/vcpkg/users/versioning#baselines),
    [versioning concepts](https://learn.microsoft.com/en-us/vcpkg/users/versioning.concepts)).
23. **Custom Git registry:** a repository with `ports/` and `versions/baseline.json`.
    Versions are added with
    `vcpkg --x-builtin-ports-root=./ports --x-builtin-registry-versions-dir=./versions x-add-version <port>`
    ([publish to a Git registry](https://learn.microsoft.com/en-us/vcpkg/produce/publish-to-a-git-registry)).
    Version entries carry a `git-tree`. Consumers add
    `{"kind": "git", "repository": …, "baseline": <commit>, "packages": [...]}` under
    `registries` in `vcpkg-configuration.json`. "Git registries are the recommended
    method for implementing a custom registry". Do not rewrite a published `git-tree`
    ([registries concepts](https://learn.microsoft.com/en-us/vcpkg/concepts/registries)).
24. **Overlay ports** are directories given by `--overlay-ports`, `overlay-ports` in
    `vcpkg-configuration.json` or `VCPKG_OVERLAY_PORTS`. They "take priority" in name
    resolution
    ([overlay ports](https://learn.microsoft.com/en-us/vcpkg/concepts/overlay-ports)).
    The pages read say nothing about overlay ports and version constraints.
25. Custom registries carry no curated policy. The guide sends ports that fail it to
    "publishing a custom registry" or "an overlay port" (tips in the
    [maintainer guide](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#curated-registry-design-goals)).
26. `GITHUB_TOKEN`: "The token's permissions are limited to the repository that contains
    your workflow"
    ([GitHub docs](https://docs.github.com/en/actions/concepts/security/github_token)).

### Review process and latency

27. New ports should be opened as draft PRs. A maintainer may close a PR after 60 days
    waiting on the contributor
    ([maintainer guide, "Use GitHub draft PRs"](https://learn.microsoft.com/en-us/vcpkg/contributing/maintainer-guide#use-github-draft-prs)).
28. The 30 most recent merged PRs matching "new port" in the title (merged
    2026-09-11 to 2026-10-24): merge time median 2 days, minimum same day, maximum
    120 days (three KDE Frameworks ports opened 2026-06-23)
    (`gh pr list -R microsoft/vcpkg --search "\"new port\" in:title is:merged"`). This
    sample is biased: it includes only PRs that merged.
29. Among recent closed, unmerged new-port PRs, the stated reason is usually maturity.
    vicroms closed [#53895](https://github.com/microsoft/vcpkg/pull/53895),
    [#53872](https://github.com/microsoft/vcpkg/pull/53872),
    [#53890](https://github.com/microsoft/vcpkg/pull/53890) and
    [#53061](https://github.com/microsoft/vcpkg/pull/53061) with a standard reply that
    links the maturity rule and suggests overlay ports or a registry.
    BillyONeal turned down an exception request on
    [#53462](https://github.com/microsoft/vcpkg/pull/53462). Authors withdrew
    [#54428](https://github.com/microsoft/vcpkg/pull/54428) and
    [#53458](https://github.com/microsoft/vcpkg/pull/53458) themselves for the same reason.
    These closures came 1–14 days after opening.

### How other game-modding SDKs are distributed

30. **CommonLibSSE-NG** (Skyrim) shipped "as their own Vcpkg ports, through the
    Color-Glass Studios Vcpkg repository". Users add a `git` registry
    (`https://gitlab.com/colorglass/vcpkg-colorglass`) with `packages`
    `commonlibsse-ng`, `-ae`, `-se`, `-vr`, `-flatrim`
    ([README](https://github.com/CharmedBaryon/CommonLibSSE-NG#via-vcpkg)). The
    repository's last push was 2024-09-04 (`gh repo view`).
31. The Color-Glass registry holds 33 ports, among them `commonlibsse-ng*`, `skse`, `f4se`,
    `commonlibf4`, `commonlibvr`, and several `*-prebuilt` ports (`qtbase-prebuilt`,
    `python3-prebuilt`, …). Its last activity was 2024-05-16. Its `commonlibsse-ng`
    portfile still uses the deprecated `vcpkg_configure_cmake`/`vcpkg_install_cmake`
    ([GitLab API, project](https://gitlab.com/api/v4/projects/colorglass%2Fvcpkg-colorglass),
    [ports tree](https://gitlab.com/api/v4/projects/colorglass%2Fvcpkg-colorglass/repository/tree?path=ports&per_page=100),
    [portfile](https://gitlab.com/colorglass/vcpkg-colorglass/-/blob/main/ports/commonlibsse-ng/portfile.cmake)).
32. The maintained **CommonLibSSE** (`libxse/commonlibsse`, pushed 2026-10-05) builds with
    XMake 3.0+ and is consumed as a Git submodule
    ([README](https://github.com/libxse/commonlibsse)). Plugins use
    `includes("lib/commonlibsse")` and its `commonlibsse.plugin` rule
    ([skse-qui xmake.lua](https://github.com/qudix/skse-qui/blob/main/xmake.lua)).
33. **Anvil's own template** consumes the SDK with `FetchContent_Declare(anvil_sdk URL …/v1.5.0/anvil-sdk-windows-x64.zip URL_HASH SHA256=…)`
    ([anvil-plugin-template CMakeLists.txt](https://github.com/happy-dwarf-team/anvil-plugin-template/blob/main/CMakeLists.txt)).
    No Anvil issue asks for vcpkg. The only mentions are in #12 and #21
    (`gh issue list -R happy-dwarf-team/anvil --search vcpkg --state all`).

## Inferences

- **Earliest curated submission: about 2027-04-15**, six months after v0.2.0 (fact 2).
  The "six months of public development" clock may start a day earlier, if the
  repository was public from creation. Its visibility history was not checked.
- **A curated port would build from source in practice.** The prebuilt rule (fact 4)
  probably allows a port that repackages the zip: `anvil.exe` is a host tool, it
  statically links toml++/Zydis privately, and the headers and config pull in no other
  port (facts 6–7 of the tree section). But the guide prefers source builds and
  devendoring (facts 4–5), and reviewers would likely ask why a source build is not
  possible. No precedent was checked.
- **What a curated source port would need upstream in Anvil** (to stay within "prefer
  options over patching"):
  1. an option to build only `anvil_core` + `anvil` + the SDK install (no
     `version.dll`, standard plugins, Manager, tests, bench);
  2. `FIND_PACKAGE_ARGS` on the toml++ declaration and a Zydis lookup that works with
     `Zydis::Zydis` (with `SAFETYHOOK_FETCH_ZYDIS` no longer forced on that path);
  3. a vcpkg-friendly install layout: config in `share/anvilplugin/`, tool path
     overridable (the config already honours `ANVIL_SDK_TOOL`), and third-party
     licenses not read from `*_SOURCE_DIR`.
  The port itself would follow protobuf's pattern (fact 18): it depends on itself as a
  host dependency, builds `anvil.exe` only when `native`, otherwise takes it from
  `CURRENT_HOST_INSTALLED_DIR/tools/happy-dwarf-team-anvil`. Its `supports` would be
  something like `"windows & x64 & !uwp & !mingw & !xbox"`.
- **Static CRT.** `anvil_add_plugin` forces `/MT` on the plugin DLL whatever the triplet.
  The SDK's headers and tool do not care about CRT linkage. An author who also takes
  *other* libraries from vcpkg must already pick `x64-windows-static` (or a custom
  triplet) to avoid CRT mismatches; a port changes nothing here. The forced
  `CMAKE_MSVC_RUNTIME_LIBRARY` in Anvil's tree would clash with `x64-windows`/`-static-md`
  triplets in a port build. Whether vcpkg's post-build checks flag a static-CRT host
  executable was not verified.
- **Release cadence.** A curated port is updated by a PR per release (facts 21, 28),
  usually reviewed within days. At the current pace (8 releases in 12 days) the port
  would lag releases. The SDK refuses a mismatched `api`, so an older port pins authors
  to an older Anvil.
- **A team registry can be updated by the release workflow.** After the release is
  published: download the zip (or the tag tarball), compute SHA512, write
  `ports/happy-dwarf-team-anvil/{vcpkg.json,portfile.cmake}`, run
  `vcpkg … x-add-version`, commit and push to the registry. Pushing needs a GitHub App
  or fine-grained token with `contents: write` on the registry repository (fact 26), a
  new secret for the Anvil repository. Authors still bump their `baseline` to get the
  new version (fact 22), much as they now edit the URL and hash.

## Options for Anvil

| Option | Effort to set up | Per-release maintenance | What authors gain |
| --- | --- | --- | --- |
| **A. Keep `FetchContent` / `find_package` only** (today) | None | None beyond the current release workflow (zip, `SHA256SUMS.txt`, attestation) | Already: one `FetchContent_Declare` with URL + SHA-256, prebuilt `anvil.exe`, attestation, works with or without vcpkg |
| **B. Overlay port** (e.g. `sdk/vcpkg/happy-dwarf-team-anvil/` in the Anvil repo, wrapping the release zip) | Small: `vcpkg.json` + a portfile with `vcpkg_download_distfile` (SHA512), installing headers to `include/anvil`, `anvil.exe` to `tools/<port>`, config to `share/anvilplugin`; one test build in CI | Version + SHA512 per release (scriptable in `release.yml`, same repo, no new secret) | vcpkg users list it in their manifest, but must copy or submodule the overlay directory. No baseline/version history |
| **C. Team Git registry** (`happy-dwarf-team/vcpkg-registry`, wraps the zip or builds from source) | Medium: new repository, port, `versions/`, CI that installs it on `x64-windows-static`; cross-repo token for automation | Automated job per release (SHA512, `x-add-version`, push); token rotation; registry CI | One `registries` entry and `"happy-dwarf-team-anvil"` in `vcpkg.json`; version history and `version>=`; binary caching. Still a baseline bump per upgrade |
| **D. Curated port** (`happy-dwarf-team-anvil` in `microsoft/vcpkg`) | Large and **not possible before ~2027-04-15**: upstream build changes (SDK-only option, `find_package` for toml++/Zydis, vcpkg layout), a from-source port with the self-host pattern, review | A PR to `microsoft/vcpkg` per release (often merged in days, sometimes weeks), plus vcpkg-wide churn (toml++/Zydis bumps can break it) | Discoverable with `vcpkg search`; no extra registry. But every author compiles `anvil.exe` + toml++ + Zydis once per host triplet, and the build is not the attested release binary |

## Open

- Whether any plugin author wants vcpkg at all. No issue asks; the trigger in #12/#21 is
  "if authors ask". The decision rests on this.
- Whether the curated maintainers would accept a prebuilt-zip port of a host tool rather
  than a source build (fact 4 allows it on paper). No comparable accepted PR was
  examined.
- Whether `"host": true` in a *top-level* manifest behaves as documented for ports
  (fact 16 does not limit it), and how `AnvilPluginConfig.cmake` would best find the host
  tool (`VCPKG_HOST_TRIPLET` + `VCPKG_INSTALLED_DIR`, or protobuf's copy-into-target
  approach). Untested.
- vcpkg post-build checks on Anvil's layout (files at the prefix root, a static-CRT
  executable on a dynamic-CRT triplet, the `cmake/` config directory). The
  build-policies page was not found at the guessed URL and was not read.
- Which MSVC version vcpkg's CI uses and whether the tree's C++23 code and `/WX` plugin
  builds pass it. Not checked.
- Whether another curated port installs files under `include/anvil/` (file conflicts are
  checked by CI). Not checked.
- If option C is chosen: whether it is a new team repository (touches
  `docs/team-touchpoints.md` in the lab) and who owns the cross-repo token.
