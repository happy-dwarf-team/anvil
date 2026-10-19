# PROTOTYPE: writing an Anvil 2 plugin end to end

Throwaway prototype for [Plugin authoring experience: hello plugin, C++ wrapper and template repository](https://github.com/happy-dwarf-team/anvil/issues/21). It answers one question: **what does writing a new plugin look like, from an empty folder to a DLL Anvil could load?** No Anvil 2 host exists yet, so the plugins build but nothing loads them.

## Layout

| Folder | Plays the role of |
| --- | --- |
| `sdk/` | what an Anvil SDK release would ship: `include/anvil/anvil.h` (draft API 2 C header), `include/anvil/anvil.hpp` (C++ wrapper), `cmake/AnvilPluginConfig.cmake` (`anvil_add_plugin`), `tools/anvil.py` (`anvil gen` and `anvil check`) |
| `template/` | what "Use this template" would give an author: `CMakeLists.txt` (3 lines that matter), `plugin.toml`, `symbols.toml`, `src/plugin.cpp`, `.github/workflows/build.yml` |
| `hello-c/` | the same plugin in plain C |

## Run it

With Visual Studio 2022 Build Tools and Python 3.11+:

```
cmake -S template -B build/template -G "Visual Studio 17 2022" -A x64
cmake --build build/template --config Release
cmake --build build/template --config Release --target hello-check
```

The result is `build/template/plugin/hello/` with `hello.dll`, `plugin.toml` and `symbols.toml`, which is the folder that goes to `<EU4>/anvil/plugins/hello/`. With `-DEU4_DIR=<game folder>` the target `hello-deploy` copies it there. With `-DEU4_EXE=<path to eu4.exe>`, `hello-check` also compares every declaration with the game file.

## Observed (2026-10-19)

- Both plugins build with MSVC 19.4x (VS 2022 Build Tools); `anvil gen` runs as a build step.
- `anvil check` against the real `eu4.exe` 1.37.5 (SHA-256 `9AD3EFE1…771A`, read only) passes for both symbols. A changed byte is reported with its RVA, and a pointer slot declared as `write` while it lives in `.rdata` is reported as well.
- A detour whose signature differs from `symbols.toml` does not compile (`C2672`), but MSVC's message talks about template deduction, not about the symbol.
- Capturing the wrapper's `Host&` in a mid-hook lambda would dangle after `load()` returns. The prototype makes `Host` a stateless value that is safe to copy.
