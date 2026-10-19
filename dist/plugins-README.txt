Put each Anvil plugin into its own folder here:

    anvil\plugins\<plugin-id>\plugin.toml
    anvil\plugins\<plugin-id>\<plugin>.dll

Plugins written for plugin API 1 (Anvil before 1.0) are skipped with "plugin
API 1: update the plugin"; install the plugin's newer release.

Delete a plugin folder to remove the plugin. An empty folder means the game
starts exactly as without Anvil. What was loaded or skipped (and why) is
written to anvil\anvil.log on every start.
