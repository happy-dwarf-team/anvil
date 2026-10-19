# Anvil

Anvil loads native plugins into Europa Universalis IV without modifying the game. This glossary fixes the words used for its parts and for the plugins built on it.

## Language

**Core**:
The part of Anvil that must work before any plugin loads or needs a view across all plugins at once, such as load order, the patch registry and plugin approval. Nothing else belongs to it.
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
