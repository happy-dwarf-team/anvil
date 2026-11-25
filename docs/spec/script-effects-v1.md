# Script effects v1 specification

`anvil-script.effects` version 1 lets a gameplay plugin add effects of its own to EU4's script language: a modder writes `chisel_set_terrain = mountain` in an event option, a decision or a scripted effect, and the game runs the plugin's code where it runs its own effects. The decisions and their reasons are in [ADR 0008](../adr/0008-plugin-script-effects.md); the static evidence is the lab's `docs/research/script-effect-statics.md` and the prototype's run is the lab's `docs/script-effect-game-test.md` (eu4-engine-lab#32, #77). Terms are those of [GLOSSARY.md](../../GLOSSARY.md).

Until the Anvil release that ships `anvil-script`, this specification is a living document: when implementation learns something that changes it, a pull request changes it. After that release it is frozen, and version 2 gets its own file.

## 1. The plugin

| | |
| --- | --- |
| Folder | `plugins/anvil-script`, released in `anvil/plugins/anvil-script/` |
| Manifest | `phase = "early"`, `affects_gameplay = false`, no dependencies |
| Symbols | 11 per build, Steam 1.37.5 and GOG 1.37.5.2 (section 7); `symbols.lock` committed and checked in CI |
| Service | `anvil-script.effects` version 1, registered in its load |
| Header | `include/anvil/anvil_script.h` (C), in the SDK |
| Safe mode | Not kept (design §10): a safe start loads no `anvil-script`, so no plugin effect is parsed |

A consumer declares the dependency and is `early` itself:

```toml
phase = "early"
affects_gameplay = true

[[dependencies]]
id = "anvil-script"
version = "^1.16"   # the first Anvil with the service (working number)
```

## 2. The C interface

```c
#define ANVIL_SCRIPT_EFFECTS_SERVICE "anvil-script.effects"
#define ANVIL_SCRIPT_EFFECTS_SERVICE_VERSION 1

/* Where an effect runs: the scope the game gives it. */
typedef struct AnvilScriptScope {
    uint32_t size;          /* set by the service */
    int32_t province;       /* the scope's province ID; 0 when the scope has none */
    char country[4];        /* the scope's country tag, "RUS"; "" when it has none */
} AnvilScriptScope;

/* One execution of one effect. Valid only during the call. */
typedef struct AnvilScriptCall {
    uint32_t size;          /* set by the service */
    uint32_t reserved;
    const char *key;        /* the registered key, lower case */
    const char *value;      /* the right-hand side as written, without quotes */
    AnvilScriptScope scope;
} AnvilScriptCall;

/* Runs where the game runs its own effects (section 4). */
typedef void (*AnvilScriptExecute)(void *context, const AnvilScriptCall *call);
/* One line for the tooltip, UTF-8, no line break. Writes at most capacity
 * bytes including the terminating zero and answers the whole length; 0 for no
 * line. The scope is the one the tooltip is built for. */
typedef uint32_t (*AnvilScriptDescribe)(void *context, const AnvilScriptCall *call, char *utf8, uint32_t capacity);

typedef struct AnvilScriptEffect {
    uint32_t size;               /* sizeof(AnvilScriptEffect) as known by the caller */
    uint32_t flags;              /* 0 in version 1 */
    const char *key;             /* "<owner>_<name>": section 3 */
    AnvilScriptExecute execute;  /* required */
    AnvilScriptDescribe describe;/* may be NULL: no tooltip line */
    void *context;               /* passed back as is; must live for the process */
} AnvilScriptEffect;

typedef struct AnvilScriptEffectsService {
    uint32_t size;
    /* From an early plugin with affects_gameplay = true, before the game
     * parses its first effect: in practice in its anvil_plugin_load. The
     * service copies the key; the callbacks and the context must live for the
     * process. */
    AnvilResult (*register_effect)(AnvilPlugin *self, const AnvilScriptEffect *effect);
} AnvilScriptEffectsService;
```

C++ (`anvil.hpp` gets nothing new): `host.service<AnvilScriptEffectsService>(ANVIL_SCRIPT_EFFECTS_SERVICE, ANVIL_SCRIPT_EFFECTS_SERVICE_VERSION)` and a plain `noexcept` function for each callback. The callbacks are called by the game through the service, not through a change, so the wrapper's adapters do not catch for them: an exception that leaves one ends the process like any exception in game code, and the crash report names the plugin's code (design §12).

Within API 2 (ADR 0001): the structs grow only by appending behind `size`; a changed meaning is version 2 next to version 1.

## 3. Registration

`register_effect` checks, in this order, and answers the first failure:

| Check | Result |
| --- | --- |
| `self` is a plugin the core knows, `effect` and its `key` and `execute` are not NULL, `effect->size` covers `execute` | `ANVIL_ERROR_INVALID_ARGUMENT` |
| The caller's manifest has `affects_gameplay = true` (from `anvil.control`) | `ANVIL_ERROR_WRONG_KIND` |
| The caller's manifest has `phase = "early"` (from `anvil.control`), and the game has not parsed an effect yet (the hook has seen no call) | `ANVIL_ERROR_WRONG_PHASE` |
| The key is 2–63 bytes of `[a-z0-9_]`, starts with the caller's id with `-` as `_`, then `_`, and has a name after it | `ANVIL_ERROR_INVALID_ARGUMENT` |
| No plugin registered the key before | `ANVIL_ERROR_ALREADY_EXISTS` |
| `flags` is 0 | `ANVIL_ERROR_UNSUPPORTED` |
| Room: at most 256 keys in all | `ANVIL_ERROR_OUT_OF_MEMORY` |

Each refusal is logged with the caller and the key. A key is never removed: nothing is unloaded in API 2.

The service cannot ask the game's lexer at registration, because an early plugin loads before the game's C runtime. A key that is a word of the game is refused at parse time instead: when the hook sees a registered key with a token other than the parser's token for a name the lexer does not know (`0xF` on 1.37.5, seen in the game on both builds; the lexer's token lookup answers `0xC`, but that is not what reaches the member reader), the key is passed to the game and the service logs once that the key is a word of the game or of its data.

## 4. Life of an effect

**Parse.** The hook on the game's member reader of effects (`effect_read_member`: effect, reader, token), the function that reads one member of an effect block, runs for every member of every container effect, on whichever thread parses: seven threads in a start of the base game, one loading thread with almost all calls [live]. It reads the key from the reader's left-hand token (`lhs`) and looks it up (a bit per first byte of the registered keys, then a case-insensitive comparison with each key; no lock: the table is complete before the first parse). For a registered key whose right-hand side (the reader's `rhs` token) is not `{`, it:

1. allocates its object from its own heap: the game's effect object (0x38 bytes) followed by the key's index and a copy of the value (up to 255 bytes; a longer value is refused, as below);
2. runs the game's effect constructor (`effect_ctor`), which sets the token to −1 and registers the effect in the game's effect database under its mutex;
3. puts its vtable over the game's: one copy of the effect's 19 slots, made once, with the deleting destructor (0), the value's assignment (9), the description (13) and the execution (15) replaced; the rest stay the game's;
4. appends the object to the parent's children through the game's array append (`array_append`), which grows the vector with the game's heap;
5. calls its own assignment through the vtable with the `rhs` token, as the game does for its simple effects, which copies the value;
6. stores the token in the effect's token field (`effect_set_token` is that one store).

A block on a registered key, and a value over 255 bytes, are passed to the game, which logs `Unknown effect type. Key: <key>` and skips them; the service adds one line to `anvil.log` per key with the reason. Every other member goes to the game unchanged.

**Execute.** The game calls slot 15 (the execution: the effect, the scope, an `int`) where it runs its own effects: an event option or a decision inside the command that every client runs, an on-action in the tick, a scripted effect wherever it is used [live: on the main thread, inside the command of an event option and of an AI decision; the `int` was 0]. The service:

1. returns at once when the owner is suspended (design §13), asked through `anvil.control` at each call;
2. fills `AnvilScriptScope` from the scope's own fields, as the game's simple effects read them: the province at `+0x28` of the scope (as the province culture effect reads it), the country's tag at `+0x44` when `+0x4B` is set and at `+0x2C` otherwise (as the treasury effect reads it), `""` when that tag is not valid. The effect's own getters of its scope's province ID and country tag are not used: they answer only for an effect's scope bits (`ROOT`, `FROM`, event targets), which the service's effects never have, and gave 0 and `---` in the game [live]. In a province scope the country is `""` [live];
3. calls the plugin's `execute`.

`anvil-game.world` answers `ANVIL_ERROR_WRONG_PHASE` inside `execute`: the game is in the middle of a command or a day. The plugin reads what it needs itself or from the call.

**Describe.** When the game builds a tooltip it calls slot 13 (the description: the effect, the scope, a `CString` prefix by value, an `int` and a `bool`). The service calls the game's base description, which frees the prefix the caller passed by value and gives an empty string. Without `describe`, or when it answers 0, that empty string is the answer, as for a bare effect object: no line in the tooltip [live]. Otherwise the line (at most 511 bytes) is converted by ADR 0007's rule as anvil-map applies it ([map-modes-v1 §7](map-modes-v1.md#7-text); clarified in anvil#305: UTF-8 kept when Mei reports UTF-8 text, Windows-1252 with `?` otherwise, cut at the first byte that is not UTF-8; literal, so `$` stays and `§`, which the game cannot escape, becomes `?`; line breaks dropped, the line is one line) and ended with a line break, as the game's own lines are (the parent indents them; without the break the next line joins ours [live]), and built as the game's string with the game's `CString` constructor from a `const char*` (`cstring_from_text`), so the game frees it with its own heap.

**Destroy.** The game deletes a parent's children through slot 0 when it drops the parent: a reload of the scripts, or never, at exit [pdb]. The service's destructor runs the game's effect destructor (`effect_dtor`, which unregisters it), then frees the object to its own heap when the flag asks for it. Nothing else of the game frees these objects [pdb]; a reload was not tried in the game.

**Saves and hot-join.** Nothing of an effect is written to a save [pdb]. A loaded save, a hot-joining player and a new game parse nothing; the effects parsed at start-up stay.

## 5. Errors and logging

- Every refusal of section 3 and every passed-on member of section 4 writes one line to `anvil.log` per key and reason, never one per member, so a mod that uses a key thousands of times costs one line.
- At `anvil-game.ready`, when `anvil-game` is loaded, one line counts the parsed effects per key; without registered keys it counts the hook's calls.
- A fault in a callback is the plugin's crash (section 2); the service does not wrap the game's calls in guards.

## 6. Threads and cost

- The parse runs on the game's parsing threads, possibly several at once. The key table is read-only after registration, and each object belongs to the thread that parses its parent, so the hook takes no lock of its own; the game's constructor takes the game's mutex.
- `execute` and `describe` run on the thread the game uses for them: the main thread in every call seen [live]. `describe` runs often (the game builds a decision's description in the background), so it must be cheap. A plugin that keeps state across calls guards it itself.
- The empty cost is a test of the key's first byte per call of the member reader, and a comparison with the registered keys that start with it: 862 474 calls in a start of the base game with a small mod [live]. The release measures the start-up with the service and no registered key against Anvil without it (design §17).

## 7. Symbols

Every address is a declared symbol; every offset is a value taken from verified bytes. RVAs are Steam 1.37.5 / GOG 1.37.5.2 (`eu4-1.37.5-steam-win64`, `eu4-1.37.5.2-gog-win64`); the bytes are the same on both builds unless the row says otherwise. `symbols.toml` verifies them by `sha256` and `size` (ADR 0003); the sizes below are those.

| Symbol | Steam | GOG | What | Values |
| --- | --- | --- | --- | --- |
| `effect_read_member` | `0x5DEC50` | `0x5DE720` | the member reader of effects (effect, reader, token), the function hook: its prologue, 18 bytes | |
| `effect_lhs_site` | `0x5E4B04` | `0x5E45D4` | in its default branch, 7 bytes that take the address of the reader's left-hand token | `lhs` (the reader's left-hand token, 0x30) |
| `effect_assign_site` | `0x5E0F43` | `0x5E0A13` | in a simple effect's case, 32 bytes (4 of them, a call target, not compared): the append to the parent's children, then the virtual call that gives the new effect the right-hand token | `children` (0x10), `rhs` (the reader's right-hand token, 0x440), `assign` (slot 0x48) |
| `effect_set_token` | `0x5EF140` | `0x5EEC10` | the effect's token setter, 4 bytes: one store and a return | `token` (0x28) |
| `effect_ctor` | `0x5DE770` | `0x5DE240` | the effect constructor: its first 20 bytes | |
| `effect_vtable_site` | `0x5DE797` | `0x5DE267` | in it, 10 bytes that load the effect's vtable and store it in the object; a digest per build, since the vtable's address differs | `vtable` (rip-relative: Steam `0x1C9E548`, GOG `0x1C872C8`) |
| `effect_dtor` | `0x5DE8D0` | `0x5DE3A0` | the effect destructor: its first 24 bytes | |
| `array_append` | `0xC9F10` | `0xC9F40` | the game's append to an array of pointers, a `std::vector<void*>` push: the whole fast path, 25 bytes | |
| `scope_province_site` | `0x62367F` | `0x62314F` | `+0x1F` in the province culture effect's execution, 10 bytes that test the scope's province | `province` (0x28) |
| `scope_country_site` | `0x5EFC55` | `0x5EF725` | `+0x25` in the treasury effect's execution, 25 bytes that read the override's flag and pick one of the two tags | `override_valid` (0x4B), `override` (0x44), `tag` (0x2C) |
| `cstring_from_text` | `0x1703550` | `0x16ED320` | the `CString` constructor from a `const char*`: its first 24 bytes | |

The vtable's slot count (19) and the slots replaced (0, 9, 13, 15) come from the PDB and are the same in both builds' vtables [pdb]; the implementation checks at load that slot 4 of the copied vtable is `effect_read_member` and slot 15 lies inside `eu4.exe`, and registers no service when either fails. The GOG build 1.37.5.0 comes as any third build does (design §5.2).

An absent symbol (§5.2) on a future build: without any of these the service is not registered and `get_service` gives NULL, so a consumer that requires it is skipped with the reason.

## 8. Tests

- Unit tests (`tests/anvil_script_plugin_tests.cpp`) with a stand-in game: a fake member reader, a fake vtable of 19 slots and a reader with its left-hand and right-hand tokens, for the key rule, registration's order of checks, the phase rule, a block and a long value passed on, suspension making `execute` a no-op, `describe` truncation and the destructor's free.
- The game test (Steam and GOG, under the lab's lock): a test plugin registers two keys, a test mod uses them in an event option, a decision, a scripted effect with a parameter and an AI decision; it checks the tooltip line, `has_terrain`-style results right after the effect, a block passed on, the plugin missing (`Unknown effect type` and only that key skipped), a `main` consumer refused, an ironman-style suspension (effect does nothing), and the start-up's empty cost.

## 9. Not in version 1

Triggers; block values and nested keys; a key from `plugin.toml` for `main` plugins; removing a key; effects in a safe start; a rule file for mod validators.
