# Forge: a design system for the Manager and the overlay (proposal)

Status: proposal, 2026-11-14; its accent and the overlay's look are decided (§ Decided).
Nothing here is built. The skin was tried as "Skin: Forge" on the
throwaway page `tools/manager/prototype/module-switches.html` of the branch
`prototype/module-switches` (variant B5; the button in the bottom bar switches between today's
colours and Forge). It changes how things look, not the rules: [ui-guidelines.md](ui-guidelines.md)
still says what is shown and when.

## Why

- **Nothing says Anvil.** Both apps use ImGui's dark style with a few colours changed
  (`src/ui/imgui_colors.hpp`). They look like a debug window, and they look the same as any
  other ImGui tool a player has seen inside a game.
- **Blue means two things.** The same blue fills frames, buttons, tabs, the selection and the
  checkbox, and it is also the info colour ("at the next start"). An info word next to a
  blue control has nothing to stand out against.
- **Every button looks the same.** "Start EU4", "Approve...", "Cancel" and "Remove" have one
  style, so a dialog does not show which button acts.
- **Pure white text and a cold grey** sit badly over EU4's map and its parchment-and-brass
  interface, which the overlay is drawn on.

## Principles

1. **Warm graphite, one accent.** The surfaces are dark greys with a little warmth. One accent,
   *ember*, marks what the player acts on: the selection, the keyboard cursor, the current tab,
   a checkbox that is on and the one main action of a view. Nothing else is ember.
2. **State colours keep their meanings** (ui-guidelines §3): good, warning, bad, info, weak.
   The accent is never a state, and a state never fills a control.
3. **Every state has a shape, every exception a word.** Colour never stands alone: a list
   entry starts with its state's glyph, and anything but "on" is also said in words (§ Status).
4. **Only what ImGui draws.** Every token is an `ImGuiCol_*` colour or an `ImGuiStyleVar`. Two
   small exceptions are drawn with `ImDrawList`: the status glyphs and the plugin's toggle
   (an `InvisibleButton` with a rounded track and a knob, about 20 lines in `src/ui/`). No new
   dependencies.
5. **Say the usual once, mark the exception.** A list or a module row does not repeat "on" or
   "applies at once"; it says what is unusual about it.

## Colours

Contrast is WCAG contrast against the panel (`#181716`), checked with a script on 2026-11-14.
Text needs 4.5:1 and a control's outline 3:1.

| Token | Hex | `ImVec4` | Use | Contrast |
|---|---|---|---|---|
| `bg0` | `#0d0c0b` | 0.051, 0.047, 0.043 | title bars, the code view, behind the window | |
| `window` | `#121110` | 0.071, 0.067, 0.063 | `WindowBg` | |
| `panel` | `#181716` | 0.094, 0.090, 0.086 | `ChildBg`: the list and the detail | |
| `raised` | `#1f1d1b` | 0.122, 0.114, 0.106 | `PopupBg`, dialogs, the current tab, hover on rows | |
| `frame` | `#242220` | 0.141, 0.133, 0.125 | `FrameBg`, the segmented track, secondary buttons | |
| `frame_hovered` | `#2e2b28` | 0.180, 0.169, 0.157 | `FrameBgHovered`, `ButtonHovered` | |
| `frame_active` | `#38342f` | 0.220, 0.204, 0.184 | `FrameBgActive`, `ButtonActive` | |
| `line` | `#2c2926` | 0.173, 0.161, 0.149 | `Separator`, `TableBorderLight`, row lines | |
| `border` | `#4a4540` | 0.290, 0.271, 0.251 | `Border`, `TableBorderStrong`, frame outlines | 1.9 (decorative) |
| `text` | `#ece8e1` | 0.925, 0.910, 0.882 | `Text` | 14.7 |
| `text_weak` | `#a39e96` | 0.639, 0.620, 0.588 | secondary text, "off", counts | 6.7 |
| `text_dim` | `#8a857e` | 0.541, 0.522, 0.494 | `TextDisabled`, placeholders | 4.9 |
| `ember` | `#e8853d` | 0.910, 0.522, 0.239 | `CheckMark` background, `NavCursor`, `TabSelectedOverline`, `SliderGrab`, primary buttons, the selection bar | 6.7 |
| `ember_hovered` | `#f09a57` | 0.941, 0.604, 0.341 | a primary button under the mouse | |
| `on_ember` | `#1a1209` | 0.102, 0.071, 0.035 | text and ticks on ember | 6.9 on ember |
| `good` | `#6cc48a` | 0.424, 0.769, 0.541 | loads, allowed | 8.4 |
| `warn` | `#e5c04b` | 0.898, 0.753, 0.294 | needs a look, kept out of this game | 10.2 |
| `bad` | `#ef6a5e` | 0.937, 0.416, 0.369 | failed, cannot load | 5.9 |
| `info` | `#7aa7e0` | 0.478, 0.655, 0.878 | waits for the next start, text buttons | 7.2 |

The selection and the active filter chip are ember at 16 % over the panel (`Header`,
`HeaderHovered` at 22 %, `HeaderActive` at 28 %). The search highlight is ember at 28 % with an
ember underline. White text on ember reaches only 2.7:1, so text on a primary button is
`on_ember`.

Ember and warning are both warm. They stay apart because ember is only ever on controls and
warning only ever on words, glyphs and the left bar of a banner, and every warning carries its
word and its ▲ (ui-guidelines §3).

## Shape and spacing

Today the two apps set their style apart, and differently: the Manager in
`tools/manager/app/main.cpp`, the overlay in `plugins/anvil-overlay/src/ui.cpp`; the rest is
ImGui's default. Forge sets one style for both in `src/ui/`.

| Style variable | Today: Manager | Today: overlay | Forge |
|---|---|---|---|
| `WindowRounding` / `ChildRounding` | 0 / 0 | 6 / 0 | 6 / 6 |
| `PopupRounding` | 0 | 0 | 8 |
| `FrameRounding` / `GrabRounding` | 3 / 0 | 4 / 0 | 4 / 4 |
| `TabRounding` | 3 | 5 | 4 |
| `FrameBorderSize` | 0 | 0 | 1 (frames are outlined in `border`) |
| `FramePadding` | 8, 4 | 4, 3 | 8, 4 |
| `ItemSpacing` | 8, 7 | 8, 4 | 8, 6 |
| `WindowPadding` | 14, 12 | 8, 8 | 12, 12 |
| `ScrollbarSize` / `ScrollbarRounding` | 14 / 9 | 14 / 9 | about 0.75 × font size / 12 (ui-guidelines §2) |
| `TabBarOverlineSize` | 1 | 1 | 2 |

All of them pass through `ScaleAllSizes`, as today.

## Type

- **Segoe UI at 17 px, as today**, and the CJK fonts as today. Windows ships it, so the
  installer does not grow and the CJK fallbacks keep matching it.
- **Segoe UI Semibold** (`seguisb.ttf`, also in Windows) as a second font for plugin names,
  the heading of the detail, group headings and the title of a dialog. One more font in the
  atlas, loaded with the same glyph ranges; for CJK the regular font stands in.
- No uppercase labels: the strings are translated, and uppercase does not exist in CJK.

## Components

| Component | Forge |
|---|---|
| **Tabs** | Flat text in `text_weak`; the current tab is `raised` with an ember overline (`TabSelectedOverline`) and `text`. A tab that needs attention keeps its count ("Plugins · 2 to approve"). |
| **Primary button** | Ember with `on_ember` semibold text. One per view: "Start EU4", the confirm button of a dialog, "Approve...". |
| **Secondary button** | `frame` with a `border` outline. Everything else, Cancel included. |
| **Text button** | No frame, `info` text: "More", "Clear", "Reset settings (2)...", "Revoke approval...". |
| **Plugin toggle** | A 36 × 20 track (in font units: about 2.1 × 1.2 lines) with a round knob and the word "On" or "Off" in semibold after it; ember track and `on_ember` knob when on, a `frame` track with a `border` outline when off. Only the plugin's own switch uses it, in the detail's header. |
| **Checkbox** | An outlined box; on, it is filled with ember with an `on_ember` tick. When its plugin is off, the fill is a muted brown (`#6b5440`), so the choice stays visible but reads as held (ui-guidelines §5). |
| **Segmented control** | A `frame` track with a 2 px inset; the chosen segment is ember at 28 % with an ember outline. |
| **Input and combo** | `frame` with a `border` outline that brightens on hover; the caret is ember (`InputTextCursor`). |
| **List row** | Two lines, always (ui-guidelines §5): a status glyph in a column of its own, the name in semibold and an `info` dot when something waits; under it one phrase in 12 px, in `text_weak` for a summary and in the state colour for an exception. Hover `raised`; the selection is ember at 16 % with a 3 px ember bar at its left edge. |
| **Section header** in the list | Small text in `text_weak` on no fill, a count in a `frame` pill, and "▲ 2" / "✕ 1" in the state colours when entries need attention ("From mods (3) ▲ 2 ✕ 1"), brighter on hover. |
| **Group header** in the detail | Semibold `text`, a fold arrow, "n of m on", a `line` rule to the right. |
| **Detail header** | The name in semibold at 17 px with version and id under it in `text_weak`; at the right the plugin toggle, a `line` separator, then Reset and Remove as secondary buttons. Under it, in the overlay, the plugin's state when it is an exception, with its glyph ("▲ kept out of this game: its author keeps it out of ironman"). It stays at the top of the scrolling detail (a fixed part above `BeginChild`), so the plugin being changed is always named. |
| **Tag and chip** | Pills with a hairline. The "changes the game" tag carries the warning colour only in its word and outline. An active filter chip is ember at 16 % with an ember outline. |
| **Banner** | `raised` with a 3 px left bar in the state colour; the text stays `text`, only its first bold words take the state colour. A list of facts inside it sits in a `panel` box. |
| **Dialog** | `raised`, rounded 8, the screen dimmed by `ModalWindowDimBg` at 66 %. The title is semibold; the primary button sits at the right with Cancel to its left. |
| **Overlay window** | `window` at 97 % opacity, rounded 8, a 2 px ember line at the top of its title bar instead of the blue title bar. |
| **Scrollbar** | No track; the grab `#3d3935`, `border_hovered` on hover, ember while dragged. |
| **Keyboard cursor** | A 2 px ember outline (`NavCursor`), always visible while navigating with the keyboard. |

### Status

Each list entry starts with a glyph of its own shape, in a column of its own, and an
exception's word in the detail starts with the same glyph. They are drawn with `ImDrawList`
at the text height, so no font has to carry them:

| State | Glyph | Colour |
|---|---|---|
| on, on in this game | tick | `good` |
| off, off: needs X, off in this game | ring | `text_weak` |
| waits for approval, update waits, kept out of this game | triangle | `warn` |
| cannot load | cross | `bad` |
| waits for the next start | filled dot | `info` |

## UX changes that come with it

1. **One primary action per view**, in ember: the player sees at once what a dialog or a card
   wants from them.
2. **The selection is a bar plus a tint** rather than a solid fill, so a selected row that is
   also greyed out or needs attention keeps its own state visible.
3. **The detail header stays put** while the modules scroll.
4. **Warning text is not written in yellow.** A banner says its state with its bar and its first
   words; the rest is body text, readable at length (the approval card today is yellow from top to bottom).
5. **Shapes for states** (§ Status), so the list reads in greyscale and for colour-blind
   players.
6. **The keyboard cursor is ember and 2 px**, easy to see on the warm surfaces.
7. **The overlay is more opaque** (97 %) and keeps its body text off pure white, so it reads
   over a bright map without glare.
8. **The plugin's switch is a toggle in the header**, apart from the modules' checkboxes, and a
   plugin without modules loses the Off of its "where it runs" control, which said the same.
9. **The list says the usual once** (review of 2026-11-14 below).

## Review of the list and the overlay, 2026-11-14

The overlay's list (248 px wide in an 820 px panel) was read entry by entry in the prototype,
in an ironman game, against the `/ui-ux-pro-max` rules on visual hierarchy, primary action,
truncation and compact labels.

| Found | Change |
|---|---|
| Every entry said "on in this game"; next to a long name the word wrapped under it, so rows had two or three lines at random. | The glyph says "on"; the second line is a summary. Entries are two lines, always. |
| Lodestone had three lines: "kept out of this game", "its author keeps it out of ironman", "changes the game". | One phrase: "▲ kept out of ironman". The full reason is in the detail's header. |
| The "From mods" header ran to three lines ("3 plugins in 3 enabled mods, 2 wait for approval, 1 cannot load"), repeating what its entries said. | "From mods (3) ▲ 2 ✕ 1". |
| Versions ("0.0.0") on every entry, needed only for an update. | In the detail and the tooltip; in the phrase when an update waits ("update 1.1.0 waits for approval"). |
| Every module row in the overlay said "on in this game", and every setting "applies at once". | Rows say only exceptions ("off in this game", "applies at the next start"); one line above says the rule. |
| "Where it runs" said "Kept out of ironman and multiplayer" and then "Its author keeps it out of ironman and multiplayer". | Each mode is named once, as the player's or as the author's choice. |
| "2 plugin(s) or module(s)". | Real plurals. |

## What came from `/ui-ux-pro-max`, and what did not

- Taken: contrast of at least 4.5:1 for text and 3:1 for the focus ring, a visible keyboard
  cursor, labels next to fields, semantic colour tokens defined once, the dense spacing
  scale for a tool, and hover feedback on every control.
- Not taken: its generated design system was a landing-page pattern with a neon purple and
  rose palette and the display fonts Russo One and Chakra Petch. That suits a game's
  marketing page, not a settings tool drawn over a game, and adding fonts would grow the
  installer and miss the CJK fallbacks. Its palette search had no dark warm palette; the Forge
  palette is our own, checked for contrast above. Its motion advice (scroll reveals,
  transitions) does not apply to an immediate-mode UI redrawn every frame.

## Rollout

- `use_dark_colors()` in `src/ui/imgui_colors.hpp` becomes `use_anvil_style()`: the colours
  above, the style variables above, and the named colours (`good`, `warn`, `bad`, `info`,
  `weak`, `ember`) as constants the two apps share. ui-guidelines §3 says they are defined
  once there; only the values change.
- The Manager and the overlay call it in place of `use_dark_colors()` and drop their own
  style lines, and both load Segoe UI Semibold next to Segoe UI.
- One pull request with screenshots before and after (ui-guidelines §7), together with the
  view model of ADR 0013 or right after it, so the player sees one change of look rather than two.

## Decided

- The accent is ember (`#e8853d`), as in the prototype (2026-11-14).
- The overlay looks like the Manager, not like the game's own interface (2026-11-14).
