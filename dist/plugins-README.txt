Put each Anvil plugin into its own folder here:

    anvil\plugins\<plugin-id>\plugin.toml
    anvil\plugins\<plugin-id>\<plugin>.dll

Plugins written for plugin API 1 (Anvil before 1.0) are skipped with "plugin
API 1: update the plugin"; install the plugin's newer release.

anvil-game comes with Anvil: it tells plugins when the game is ready and when
a game is loaded. Plugins that change the game need it; Anvil takes their
changes out of ironman and multiplayer games unless you allow them in
anvil\anvil.toml.

Delete a plugin folder to remove the plugin. Without plugin folders the game
starts exactly as without Anvil. What was loaded or skipped (and why) is
written to anvil\anvil.log on every start.

When the game crashes, Anvil writes a report to anvil\crashes\ (the last 10
are kept): where the crash happened and which plugins were involved. After two
starts in a row that ended before the game was ready, the next start loads no
plugins (safe mode); the one after that loads them again.
