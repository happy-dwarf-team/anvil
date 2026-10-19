# Anvil

Anvil loads native plugins into Europa Universalis IV without modifying the game. This glossary fixes the words used for its parts and for the plugins built on it.

## Language

### Anvil and plugins

**Core**:
The part of Anvil that must work before any plugin loads or needs a view across all plugins at once, such as load order, the registry of changes and plugin approval. Nothing else belongs to it.
_Avoid_: loader (for the whole of Anvil), framework

**Library plugin**:
A plugin that offers services to other plugins.
_Avoid_: shared plugin, dependency plugin, framework plugin

**Standard plugin**:
A library plugin maintained by the Anvil project and released together with Anvil. It loads, fails and can be disabled or replaced like any other plugin.
_Avoid_: built-in plugin, core plugin, official module

**Service**:
A named set of functions with a major version that one plugin publishes under its own id and other plugins look up.
_Avoid_: interface, API (for one plugin's offer), export

**Gameplay plugin**:
A plugin that declares it can change the simulation's state or results. Anvil takes its changes out of ironman and multiplayer games unless both the plugin's author and the player allow it there.
_Avoid_: cheat plugin, unsafe plugin

**Game session**:
One loaded save or new game while it runs, from the moment it is playable until the player leaves it or loads another; it is ironman, multiplayer, both or neither.
_Avoid_: game (for this), campaign, match

**Suspension**:
The state of a plugin whose changes are all taken out of the game for a while, keeping their place, and come back unchanged when it is resumed.
_Avoid_: disabling (that lasts until the next start), unloading, pausing

### Changing the game

**Symbol**:
A game address that a plugin declares together with what must be found there, such as the expected bytes, the kind of memory or the value a pointer holds.
_Avoid_: offset, signature, address (for a declared one)

**Change**:
One modification of the game's code or data by a plugin: a function hook, a mid hook, a block replacement, a call redirect, a byte patch, a pointer swap or an import replacement.
_Avoid_: patch (as the general word; a byte patch is one kind of change), modification, tweak

**Hook**:
A change that runs plugin code when the game reaches a symbol: a function hook at the start of a function, or a mid hook at any instruction.
_Avoid_: detour (for the whole change), interception

**Block replacement**:
A change that runs plugin code instead of a declared block of game instructions, which never run, and then resumes at the block's end or at another declared symbol.
_Avoid_: mid hook (for this; a mid hook lets the original code run), code cave, skip hook

**Chain**:
The ordered changes of several plugins at one address, each passing control or the previous value to the next.
_Avoid_: stack, hook list

### When the game crashes

**Crash report**:
What Anvil writes when the game crashes: where it happened and which plugins were involved, on one of four levels (in a plugin's code, in a plugin's change, a plugin on the stack, no plugin involved). It never says which plugin caused the crash.
_Avoid_: crash log, blame, culprit

**Safe mode**:
A start that loads no plugins except the standard ones needed to explain it, because the two starts before it ended before the game was ready.
_Avoid_: recovery mode, failsafe, disabled mode
