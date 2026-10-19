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
