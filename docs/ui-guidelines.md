# Player UI guidelines

Rules for everything a player sees from Anvil: AnvilManager.exe, the in-game overlay
(`anvil-overlay`) and the panels plugins draw in it. Both apps are Dear ImGui on the dark
style of [`src/ui/imgui_colors.hpp`](../src/ui/imgui_colors.hpp); design §15 says what each
one shows, this document says how. A pull request that changes a window, a tab, a panel or a
player-facing string follows it, and its description says which rule it departs from and why.

The player is in the middle of a game or about to start one. Every screen answers, in this
order: what does this do to my game, is it on now, and when does my change take effect.

## 1. Words

- **A label is a name, not a sentence.** A control's label has at most about four words
  ("Allow in ironman", "Remove"). What it means goes on a second line in weak text, or once
  at the top of the section when every row means the same.
- **Say a policy once.** A rule that holds for a whole list ("plugins that change the game
  are kept out of ironman and multiplayer unless you allow them") is a line above the list,
  not a sentence repeated in every row.
- **Player names first, ids second.** The main label is the name from `plugin.toml` or
  `[[modules]]`. Ids (`bellows.trust`), manifest keys (`affects_gameplay`) and `anvil.toml`
  lists go in the tooltip or a details line, for authors and bug reports.
- **Point to the control, not the file.** Where an app has a switch for something, a
  message says where it is ("allow it in Plugins, Smithy"), not which `anvil.toml` list to
  edit. The log keeps the file-level wording.
- **One word per idea**, in both apps, the catalog, the website and the Workshop text:

  | Word | Means | Not |
  |---|---|---|
  | on / off | the player's switch for a plugin or a module | enabled, active, checked |
  | changes the game | `affects_gameplay = true` | gameplay plugin (in player text) |
  | kept out of this game | suspended by the ironman or multiplayer rule | disabled, blocked |
  | allowed in ironman / multiplayer | the player's opt-in | whitelisted, permitted |
  | at the next start | takes effect when EU4 starts again | after restart, on reload |
  | applies at once | a `restart = false` setting, taken by the running game | live, hot |
  | kept | a choice that stays while what it belongs to is off | saved, remembered |
  | skipped / failed | did not load: a check refused it / it crashed or returned an error | broken |

- **Translatable as a whole.** Every string goes through `tr` / `trf` as a complete sentence
  with `{0}` placeholders; never glue translated fragments together. Leave room for German
  (about 40 % longer than English) and for CJK line breaking: widths come from
  `CalcTextSize` of the translated text, as the overlay's "Next start" column does.
- **Wrap, do not clip.** Text that carries meaning (a name, a description, a reason) wraps
  to the width it has. A longer explanation sits behind a "More" link or in a tooltip, never
  behind an ellipsis the player cannot open.

## 2. Layout

- **The Manager is a sidebar and a page.** Its window keeps the system's title bar, drawn in
  the Manager's own colours, and its icon. A sidebar on the left holds "Play EU4" on top,
  the game it starts, the pages a player uses (Anvil, Plugins, Catalog), the tools under
  their heading (Benchmark, Crashes, Log) and About at the bottom; the page beside it starts
  with the notices that concern every page and its title. Below about 53 font sizes of
  width the sidebar folds to a rail of icons, each named in its tooltip. The overlay keeps
  its tabs: it is a panel in the game, not a window of its own. The shell is described in
  [ui-design-system.md](ui-design-system.md), "Shell".
- **One column, one kind of content.** In a table, a state column holds the state word and
  its reason; switches have their own column, actions their own. A cell that collects a
  state, problems, an update note and a row of checkboxes is a sign the row needs columns or
  child rows.
- **Nothing jumps.** A control that depends on another one stays in place and is disabled
  with a reason in its tooltip; it is not hidden. Turning something off greys out what
  belongs to it and keeps it as it was (§5).
- **Same meaning, same place.** Checkboxes that mean the same thing line up vertically from
  row to row. Do not chain checkboxes of different meaning with `SameLine`.
- **Reveal step by step.** What a player changes often is on the first screen; what an
  author or a tester changes is one click away ("Advanced (n)"), never mixed in.
- **The window does not grow with its content.** The Manager fills its window and the
  overlay's panel keeps the size the player gave it, inside the screen. A long list or a
  long detail scrolls in its own child window (`BeginChild`), as the overlay's settings list
  does today: the list of plugins and the detail of the selected one scroll apart, a
  redraw keeps each one's scroll position, and selecting another plugin starts its detail
  at the top. Fixed lines (the pending count, the approvals line, the search field) stay
  above the scrolling part.
- **Scrollbars are quiet.** ImGui's default bar is wide and grey on a lighter track. The
  shared style in `src/ui/` sets it once for both apps: `ScrollbarSize` about 0.75 of the
  font size, `ScrollbarRounding` at its maximum, `ScrollbarBg` transparent, the grab in the
  colour of the table borders (about `0.31, 0.31, 0.35`), lighter on hover (about `0.43,
  0.43, 0.50`) and the active blue of a frame (`FrameBgActive`) while dragged. A window does
  not set its own scrollbar style.
- **Sizes in font units.** Widths, wrap positions and modal sizes are multiples of
  `GetFontSize()` (wrap text at 30 to 40 of them), never pixels, so DPI scaling and the CJK
  fonts keep the layout. Check a change at 100 % and 150 % scaling.

## 3. State, colour and feedback

- **Colours have meanings, and only these.** Good (loads, allowed), warning (needs a look,
  kept out of this game), bad (failed, will not load), info (waits for the next start), weak
  (off, kept, secondary text). They are defined once, in `src/ui/`, and shared; a tab does
  not define its own `ImVec4`. Their values, the accent and the style of the controls are
  proposed in [ui-design-system.md](ui-design-system.md).
- **Never colour alone.** Every coloured state also has its glyph of its own shape, and every
  state but the usual "on" its word, so it reads the same to a colour-blind player and on a
  screenshot in grey. Text stays at a contrast of at least 4.5:1
  on the window background, including weak text over the game in the overlay.
- **A state comes with its reason**, in player words: "kept out of this game: not allowed in
  ironman", "off with its plugin", "its author keeps it out of multiplayer".
- **One timing marker.** A switch or an opt-in always takes effect at the next start. A
  setting takes effect at once or at the next start as its `restart` says. In the overlay one
  line says once that settings apply at once, and only a setting that waits for the next start
  says so on its row; the Manager, with the game not running, marks neither. A change that waits is marked next to the control ("at the next start", and
  in the overlay "now: <value>") until the game starts; a line at the top of the tab counts
  the waiting changes and offers to undo them. A change is marked where it was made: turning
  a plugin off marks the plugin, not each of its modules again.
- **Every action answers.** A click that writes a file, approves, removes or downloads ends
  in a visible result where the player clicked: a message, a changed row, or an error that
  says what happened, what it means for the game and what to do next.
- **Notices are for decisions.** The overlay opens a notice only when the player should do or
  know something now (safe mode, an approval waiting, a plugin kept out of an ironman game
  by opt-in). One notice per issue, closable, and not shown again in the same session once
  closed.

## 4. Interaction

- **Confirm only what is hard to undo, a matter of trust, or more than the player clicked**:
  removing a plugin, uninstalling, approving native code from a mod, turning on a chain of
  dependencies, resetting more than one value. The question lists what will change. The confirm button names the action ("Remove",
  "Approve"), never "OK" or "Yes", and Cancel sits next to it.
- **One click, one thing.** A control changes one plugin, module or setting. There is no
  switch for a whole group of modules: a group header folds and counts ("7 of 8 on"), it does
  not turn anything on or off.
- **"..." means another step.** A button whose label ends in "..." opens a dialog or a
  confirmation; one without it acts at once.
- **Tooltips add, they do not hold.** Anything the player needs to decide is visible without
  hovering. Tooltips carry ids, paths, authors and longer explanations.
- **Keyboard.** The Manager keeps ImGui's keyboard navigation on: every control is reachable
  with Tab and the arrows and acts on Enter or Space. The overlay takes the keyboard only
  while one of its text fields is active, and never steals focus from the game.
- **The overlay serves the game.** It does not pause the game, does not open itself at start
  except for a notice from §3, keeps its windows inside the screen and remembers where the
  player put them.

## 5. Plugins, modules and settings

One model, drawn the same way in the Manager's and the overlay's Plugins tab. The shared
view model lives in `src/ui/` (ADR 0013), so the two apps cannot drift. The prototype that
settled it is variant B5 of `tools/manager/prototype/module-switches.html` on the branch
`prototype/module-switches` (throwaway, never merged).

### The tree

- **Plugin, then its modules, then their settings, in one place.** There is no separate
  Settings tab: a plugin's general settings (no `module`) sit under the plugin, a module's
  settings under the module. The files stay apart (`anvil.toml` for switches and opt-ins,
  `anvil/data/<id>/settings.toml` for values); the player does not need to know that.
- **List and detail.** The left list holds the player's plugins, then "From mods", then
  "Comes with Anvil" (folded unless one of them is selected). A section header is its name
  and a count, plus "▲ 2" and "✕ 1" when entries in it need attention; the entries say why.
  The detail shows the selected plugin.
- **A list entry is two lines, always.** The first: a status glyph in its own column (✓ on,
  ○ off, ▲ needs attention, ✕ cannot load), the name, and a dot when something waits for
  the next start. The second: one phrase, the exception if there is one ("off: needs Mei",
  "waits for approval", "update 1.1.0 waits for approval", "kept out of ironman"), else a
  summary ("20 of 25 modules on · 2 changed", "4 settings", "changes the game"). The version
  is in the detail and in its tooltip. While searching, a third line says where the match
  was found. A plugin that does not load is greyed.
- **Usual states are not repeated.** "On" and "on in this game" are what the glyph says; the
  words are kept for exceptions, on the list's second line and on a module's row. The overlay
  says once, above its list, "As in this game · ● waits for the next start".
- **Module groups** (`group` in `[[modules]]`) are subheadings in the order of their first
  module: a fold arrow, the name and "n of m on". A plugin with more than 12 modules starts
  with its groups folded. A group header has no switch.

### Find and filter

- **A search field above the list**, with a visible label "Find" (a placeholder alone
  disappears as the player types). It looks in plugin names and ids, module names, setting
  labels and descriptions, ignoring case. `Ctrl+F` in the Manager puts the cursor in it;
  `Esc` in the field clears it. The overlay has the same field and takes the keyboard only
  while it is active (§4).
- **A match says where it was found.** A plugin found by its name shows as usual; one found
  inside shows "found in module: Frame pacing at speed 5" or "found in setting: …" under its
  entry. The matching text is highlighted in the list and the detail, the matching
  module's row is marked, and folded groups and settings that hold a match open.
- **Filters are chips under the field**: Needs attention, Changes the game, Changed
  settings, Waits for the next start, Off. Each toggles, shows how many plugins it holds, and
  is shown only while it holds one or is on. Chips combine with each other and with the
  search: a plugin shows when it passes all of them. The chips wrap onto a second line
  rather than being clipped.
- **While searching or filtering**, the list shows only what matches, sections without a
  match are hidden and the others say "n found", and a line under the chips counts "x of y
  plugins" with a Clear button. With nothing found, the list says so in words ("No plugin
  matches “zzz” with these filters."), says where the search looks, and offers "Clear the
  search and filters".
- **Searching never changes the selection** or a choice; the detail of a plugin hidden by
  the search stays open until the player selects another. The search and the chips stay
  while the app is open and start empty at the next start.

### Switches and opt-ins

- **The plugin's own switch is a toggle in the detail's header**, right of its name, with
  its state as a word ("On", "Off"). It decides about every module, so it is bigger than
  and drawn apart from their checkboxes. Reset and Remove sit after it, behind a separator.
  It stays in sight while the modules scroll.
- **A module that does not change the game** has a checkbox: on or off.
- **A module that changes the game** has one choice of where it runs, as a segmented
  control: Off, Normal games, + Ironman, + Multiplayer, Everywhere. **A plugin without
  modules that changes the game** has the same control without Off, since its toggle turns
  it off; while it is off the control keeps showing the choice, greyed. A choice its
  author does not offer is disabled with "its author keeps it out of ironman" in its
  tooltip. The line under it says the choice in a sentence and names each kept-out mode
  once, as the player's choice or as its author's ("Runs in normal games. Kept out of
  multiplayer unless you allow it. Its author keeps it out of ironman.").
- One line above the modules explains the rule once: plugins and modules that change the
  game are kept out of ironman and multiplayer games unless you allow them; in multiplayer
  the players compare the overlay's hash of the ones that act.

### Off means kept

- **A module that is off** keeps its settings: they fold into "Settings (n), kept while it
  is off" and open greyed. Its opt-ins stay as they were.
- **A plugin that is off** greys its modules and keeps showing each module's own switch,
  opt-ins and settings, so the player sees what comes back when the plugin is turned on. A
  line says so once: "The plugin is off, so none of its modules load. Each module keeps its
  switch and its settings." The modules' controls are disabled, not unticked.
### Dependencies

- **A plugin others need** says so next to its switch: "Needed by Anvil Map. Turning it off
  stops it too; it keeps its own switch." Turning it off asks nothing: it is one click to
  undo, and the dependents show at once, greyed, why they do not load.
- **A plugin whose dependency does not load** says through what, with a way out: "Needs Mei
  (through Anvil Map), which is off, so it does not load. [Turn on Mei and Anvil Map...]".
  Its modules are greyed and kept, as under a plugin that is off.
- **Turning on a plugin whose dependencies are off asks first** and names the whole chain,
  dependencies first, each with what needs it; the button says how many plugins it turns on
  ("Turn on 3 plugins").
- **The overlay cannot turn off itself or what it needs.** Those switches are disabled in
  the overlay with "The overlay needs it. Turn it off in Anvil Manager."

### Plugins from mods

- There is no Mods tab: plugins from mods are a section of the Plugins list, headed by the
  playset ("3 plugins in 3 enabled mods, 2 wait for approval, 1 cannot load"), with a
  warning line above the list when the playset cannot be read.
- **What waits for the player shows from anywhere.** In the Manager the sidebar's Plugins
  entry carries "▲ 2" in the warning colour, whose tooltip says "Plugins (2 to approve)"; in
  the overlay the Plugins tab's label says "2 to approve". The page opens with a line naming
  each plugin that waits, an update as
  "Trade Tweaks (update to 1.1.0)", each a button that selects it. The list entry of an
  update says "update waits for approval" and which version runs until then.
- A plugin that cannot load (invalid, a conflict between mods, a local plugin with its id
  loads instead) is an entry with "cannot load" and the reason and the way out in its detail.
- A plugin from a mod waiting for its first approval shows the facts the player decides on
  (mod, Workshop item, authors, source, what changes the game, game addresses, folder hash),
  a warning that it is native code, its modules greyed, and "Approve...", which confirms.
- An update waiting for approval shows the same above the plugin's normal controls, with
  which version runs until then. An approved plugin has "Revoke approval..." under its name.
- Approving and revoking take effect at the next start and are marked like any other change.
  A plugin from a mod has no Remove: the player disables its mod in the launcher.

### Settings

- **A module that is on shows its settings at once**, under it. Settings marked advanced
  (author and lab options) fold into "Advanced (n)".
- **Each setting** shows its label, a "(?)" with its description and its default, and
  "applies at once" or "next start".
- **A changed value says so in words**: "changed", "default: 100" and Reset next to it. Reset
  of one value acts at once, with no question.
- **Reset of more than one value confirms**: "Reset settings (n)..." on a module and "Reset to
  defaults..." on a plugin list every change, old value to new, before they act. A plugin's
  reset covers its settings, its modules' switches and its opt-ins; never the plugin's own
  switch or an approval. The list entry counts changed settings ("2 changed").

### In the game

- **The overlay edits what the Manager edits**: switches, opt-ins, approvals and settings.
  Only one app writes at a time: while EU4 runs, the Manager is read-only and says so, and
  the overlay writes through `anvil.control`. Both mark what waits for the next start against
  the core's record of the last start (ADR 0013), so the count and Undo work in both.
- **The overlay shows two states**: the state of this game ("on in this game", "kept out of
  this game: not allowed in ironman", "off with its plugin in this game") and, next to it,
  what waits for the next start. A line at the top names the kind of game and the
  multiplayer code of what changes the game and acts; outside a game (the menu, the
  multiplayer lobby) it shows the code a multiplayer game started now would have, so
  players compare it before they start.
- **Settings that count for the game are locked in a multiplayer game**, from its start to
  its end, disabled with "locked in a multiplayer game". In the lobby they can still change.
- The approval prompt of a plugin from a mod lists the modules with the same words and tags.

## 6. For plugin authors

How a plugin's manifest makes this UI work. `anvil check` enforces what it can.

- **A feature the player turns on separately is a module**, not a bool or an "off / on"
  setting. The module's switch is the only switch: no setting repeats it, and no setting
  repeats an opt-in (a "game calls in ironman" setting is a module with its own
  `allow_ironman`). What a module needs
  beyond on or off (a budget, an interval, a scope) is a setting with `module = "<id>"`.
- **Lab and author tools belong together.** Timing profiles, self-checks and debug output go
  into one module, off by default (for example "Diagnostics"), with its options as advanced
  settings. A player's module then has nothing a player should not touch.
- **Do not merge what a player may need to turn off alone.** The first advice for a crash or
  a wrong result is "turn X off"; keep X its own module and use groups for order.
- **Names and descriptions.** A module's name has two to four words. Its description is one
  sentence of what the player gets, about 100 characters at most, without the group's name
  ("Simulation: ...") and without how it is built; the details go into the README. The same
  holds for settings' labels and descriptions.
- **Safe defaults and live settings.** A module that may surprise a player is off by
  default. Prefer `restart = false` where the plugin can take a new value at once.
- **Stable names.** Module ids and setting names are what players' files hold; renaming one
  loses the player's choice. A setting a module replaces is listed under `[retired.<name>]`
  (ADR 0013), so the core turns the module off for a player who had the setting off.
- **Settings that change the result** of a module or plugin that changes the game count
  for the multiplayer code and are locked in a multiplayer game (ADR 0013). Mark the others
  `affects_gameplay = false` (keys, logging, drawing), so the player can still change them.
- **Translations** cover module names and descriptions, setting labels, descriptions,
  groups and enum options (design §5.4).

## 7. Checklist for a pull request

- [ ] Labels are short names; explanations are said once; ids are in tooltips.
- [ ] The words match the table in §1; text wraps, nothing meaningful is clipped.
- [ ] No state is shown by colour alone; colours come from the shared definitions; every
      state has its reason.
- [ ] Dependent controls are disabled with a reason, not hidden; what is off is kept and
      greyed; the layout does not jump.
- [ ] Every control changes one thing; no group switches.
- [ ] Long lists scroll inside the window with the shared scrollbar style; the plugin list
      keeps its search field and filters above it.
- [ ] Choices that wait for the next start are marked where they were made.
- [ ] The Manager and the overlay draw the change from the same view model.
- [ ] Dependencies: what stops with a plugin is named; turning on a chain asks first.
- [ ] Changed settings show their default; a reset of more than one value confirms.
- [ ] Screenshots before and after, in English, German and one CJK language, at 100 % and
      150 % scaling, from the screenshot mode (`AnvilManager.exe --screenshot`, ADR 0013).
      CI keeps the Manager's as the artefact `anvil-manager-screenshots-<sha>`, drawn from
      the folder `tools/manager/screenshots/fixture.py` lays out, and one set in a narrow
      window (`--width 820`) for the sidebar's rail; the overlay's are taken in the game until
      its test host exists.
