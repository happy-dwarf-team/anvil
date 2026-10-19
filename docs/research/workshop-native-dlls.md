# Steam Workshop rules for native DLLs and their effect on checksum, ironman and achievements

Research note for [issue #16](https://github.com/happy-dwarf-team/anvil/issues/16),
input to stage 3 in [`docs/design.md`](../design.md) section 9 (plugins found in the
`anvil/` subfolder of mods enabled in the current playset). Researched 2026-10-19
against EU4 1.37.5 (Steam, Windows x64, `eu4.exe` SHA-256 prefix `835bfdf8`). The game
was not run for this note.

Sections: **verified facts** (each with its source), **inferences** (reasoning from the
facts, not observed), **open questions and experiments** (things only a test can settle).

## Short answer

- Neither Valve nor Paradox has a written rule against DLLs in Workshop items. Valve's
  rules forbid *malicious* software and cheats; Paradox's User Agreement allows
  non-commercial mods and forbids cheats. Both can remove any item at their discretion.
- Steam does not strip non-game files from EU4 Workshop items (observed locally:
  `.py`, `.lnk`, `.zip`, `.git` packs arrive intact). Other Steam games ship native DLLs
  through the Workshop (Arma 3 / ACE3). Whether the Paradox launcher's uploader
  filters `.dll` is not documented and needs a test upload.
- A Workshop item installs into `steamapps/workshop/content/236850/<id>/`, never next to
  `eu4.exe`. So the Workshop cannot install Anvil's `version.dll`; it can only carry
  plugins that an already installed Anvil picks up. That placement limit, not a ban,
  is the most likely reason Mare Liberum says the Workshop "can't host" its DLL.
- EU4's checksum covers only the folders and extensions listed in the game's
  `checksum_manifest.txt` (`common`, `events`, `missions`, `decisions`, `history`,
  `map`). A mod that only adds `anvil/` and `descriptor.mod` should leave the checksum,
  and therefore ironman achievements and multiplayer compatibility, unchanged. This
  is an inference; the experiment below settles it.
- Consequence for Anvil: the checksum will not reveal a plugin that changes gameplay.
  Ironman achievements and multiplayer checks would still pass. That is a policy
  question for Anvil (Paradox forbids distributing cheats), not a technical guard.

## Verified facts

### Valve / Steam

1. A Workshop item is a folder of files and the game decides what the content means.
   The Steamworks implementation guide describes an item as "a representation of a
   folder of files", uploaded with `ISteamUGC::SetItemContent`. It lists no file-type
   restrictions and no guidance on executable content. Items stay hidden until the
   author accepts the Workshop legal agreement.
   [Steamworks: Workshop implementation](https://partner.steamgames.com/doc/features/workshop/implementation)
2. The Steam Subscriber Agreement section 6.B lets Valve "restrict or remove Workshop
   Contributions for any reason". Section 4.B forbids creating or using Cheats, defined
   as giving an unfair competitive advantage in multiplayer. It also forbids tampering
   with the execution of Steam or Content and Services unless Valve authorised it. The
   SSA has no clause on executables or DLLs in Workshop items.
   [Steam Subscriber Agreement](https://store.steampowered.com/subscriber_agreement/)
3. The Steam Online Conduct rules forbid "uploading or linking to malicious software
   such as a virus or Trojan horse" and running cheat programs. They say nothing about
   executables as such.
   [Steam Online Conduct](https://store.steampowered.com/online_conduct/)
4. No Valve document describing automatic malware scanning or `.dll`/`.exe` stripping
   of Workshop content was found. Wallpaper Engine's developer, removing executable
   "Application" wallpapers from its Workshop on 2026-06-29, wrote that "executable
   files cannot be reliably secured with automated systems". The developer also wrote
   that the Steam Workshop "does not have a feature to lock items from being updated",
   so a hijacked author account could push a malicious update. The developer made this
   decision; it does not cite any Valve rule. Before the removal, `.exe` wallpapers had
   been distributed through the Workshop for years.
   [Wallpaper Engine 2.8.42 announcement](https://store.steampowered.com/news/app/431960/view/699894448411116924)
   (body read via the Steam news API, `ISteamNews/GetNewsForApp`, appid 431960)

### Paradox

5. Paradox's User Agreement (last update 2026-01-21), section 5, grants the right to
   "create, enjoy and make publicly available UGC, for non-commercial purposes". The
   creator is responsible for the content, and Paradox "may restrict or remove your UGC
   at our sole discretion". Section 7 (Rules of Conduct) forbids developing,
   distributing or selling cheats. The agreement has no clause on reverse engineering,
   on executable mods or on DLLs.
   [Paradox User Agreement](https://legal.paradoxplaza.com/eula)
6. The Paradox forum's "Rules for user-made mods and edits of PDS games" thread could
   not be read: it is behind a browser challenge and the archive is not reachable from
   here. Its contents are **not verified** in this note.
   ([thread](https://forum.paradoxplaza.com/forum/threads/updated-14-06-18-rules-for-user-made-mods-and-edits-of-pds-games.708039/))
7. Paradox publishes a game whose Workshop mods are executable code: in Cities:
   Skylines, mods are C# compiled to a DLL and shared through the Workshop. Paradox's
   wiki warns that "the code in Mods for Cities: Skylines is not executed in a sandbox".
   That code is managed, not native, and the game itself loads it.
   [Cities: Skylines wiki, Modding API](https://skylines.paradoxwikis.com/Modding_API)

### EU4 checksum, ironman, achievements (local install and wiki)

8. The checksummed set is listed in the game's own `checksum_manifest.txt` (read from
   the 1.37.5 install). It is limited to: `common` (`.txt`, `.lua`, recursive),
   `events`, `missions`, `decisions`, `history` (`.txt`, recursive), `map`
   (`.txt`, `.map`, `.bmp`, `.csv`, top level only), and `map/random` (`.lua` top level;
   `.txt`, `.bmp` recursive). `eu4.exe` contains the string `checksum_manifest.txt` and
   the RTTI names `CCreateFileListFromManifestTask` and `CCreateChecksumTask`. No other
   folder (`gfx`, `interface`, `localisation`, `music`, or anything unknown such as
   `anvil/`) is listed.
9. The wiki confirms that "some files and folders are ignored by the checksum
   generator". Since 1.14, ironman can be played with a non-matching checksum, but
   achievements need the correct checksum for the version. In multiplayer, a differing
   checksum makes the games incompatible.
   [EU4 wiki: Checksum](https://eu4.paradoxwikis.com/Checksum)
10. Achievements need ironman, the correct checksum, Normal or harder difficulty,
    historical lucky nations, "Limited country forming", normal province values and
    normal or historical nation setup. The wiki adds: "Most graphical, interface or
    music mods don't alter the checksum and thus are allowed."
    [EU4 wiki: Achievements](https://eu4.paradoxwikis.com/Achievements),
    [EU4 wiki: Ironman](https://eu4.paradoxwikis.com/Ironman)
11. The game's English localisation (1.37.5) lists the achievement blockers, including
    `ACHIEVEMENTS_DISABLED_MODIFIED_GAME` ("EU4 is running a mod or is altered in other
    ways") and `ACHIEVEMENTS_DISABLED_MULTIPLAYER` ("Achievements cannot be earned in
    Multiplayer"). The checksum is shown to players as the "Version" (`CALCULATING_CHECKSUM`:
    "Calculating Version...").

### How Workshop mods reach the game (local install)

12. The launcher writes a descriptor `Documents/Paradox Interactive/Europa Universalis
    IV/mod/ugc_<id>.mod` for each subscribed item, with `path=` pointing into
    `C:/Program Files (x86)/Steam/steamapps/workshop/content/236850/<id>` and
    `remote_file_id="<id>"`. The enabled playset is
    `dlc_load.json` → `enabled_mods: ["mod/ugc_<id>.mod", …]`. Steamworks exposes the install
    folder through `ISteamUGC::GetItemInstallInfo`; the game folder is never a target.
    (Observed on the lab machine; Steamworks reference above.)
13. Steam delivers non-game files of EU4 Workshop items unchanged. Across 51 installed
    items there are `.py` scripts (item 3379313816), a Windows `.lnk` shortcut
    (2967823492), `.zip` archives (2971812754, 3379313816), `desktop.ini`, `.bak`, and
    whole `.git` object packs (3327090985, 3424791106). None of the 51 contains a `.dll`
    or `.exe`. This absence does not show that `.dll` files are blocked.
14. The wiki's mod-structure page documents uploading through the launcher's "Upload
    Mod" button for a folder with `descriptor.mod`. It names no excluded file types.
    [EU4 wiki: Mod structure](https://eu4.paradoxwikis.com/Mod_structure)

### Precedents

15. **Arma 3 / ACE3** ships native extension DLLs through the Steam Workshop. ACE3's
    HEMTT build config includes `"*.dll"` and `"*.so"` in the built mod. Its install
    guide's main route is "Subscribe" on the Arma 3 Workshop (item 463939057, app
    107410). Arma's engine itself loads extensions from mod folders, so no external
    loader is needed.
    [ACE3 `.hemtt/project.toml`](https://github.com/acemod/ACE3/blob/master/.hemtt/project.toml),
    [ACE3 installation guide](https://ace3.acemod.org/wiki/user/installation-guide)
16. **EU4DLL** (double-byte font patch) splits distribution. Its `version.dll`/`d3d9.dll`
    come from GitHub releases or an installer and go "into the game directory"; the
    language mods that rely on them are subscribed on the Workshop.
    [matanki-saito/EU4dll README](https://github.com/matanki-saito/EU4dll)
17. **Mare Liberum (rdavislee/eu4-per-good-trade)** ships its DLL (`d3dx9_43.dll`, next
    to `eu4.exe`) only through GitHub releases. The README says the native plug-in is
    something "Steam Workshop can't host"; WHY.md says the same. Neither file gives a
    reason or cites a rule. Its DLL reads `dlc_load.json` and stays dormant unless its
    mod is enabled, the same gating Anvil's stage 3 plans. Its mod also ships
    trade-node script files, so it changes the checksum anyway ("achievements off, as
    with any mod").
    [README](https://github.com/rdavislee/eu4-per-good-trade#readme),
    [INSTALL.md](https://github.com/rdavislee/eu4-per-good-trade/blob/main/INSTALL.md),
    [WHY.md](https://github.com/rdavislee/eu4-per-good-trade/blob/main/WHY.md)

## Inferences

- **No ban, but no protection.** Nothing found forbids a non-malicious DLL in an EU4
  Workshop item, and DLLs travel through the Workshop in other games. The risks are
  discretionary: Valve (SSA 6.B) or Paradox (UA §5) can remove an item, and antivirus
  heuristics on the user's machine can quarantine it.
- **Why Mare Liberum "can't" use the Workshop.** Its DLL must sit next to `eu4.exe`,
  and a Workshop item can only put files under `workshop/content/236850/<id>`
  (facts 12, 17). Anvil has the same limit for `version.dll`. Plugin DLLs loaded by
  Anvil from a mod's `anvil/` folder do not have it.
- **Checksum.** From fact 8, a mod containing only `descriptor.mod`, `thumbnail.png`
  and `anvil/**` adds no file to the checksummed set. The checksum, ironman
  achievements and multiplayer compatibility should match vanilla. Two caveats:
  - A `replace_path` line for a checksummed folder, or any extra `.txt` under
    `common/`, `events/` and similar folders, changes the checksum even if the mod is
    "only Anvil". A Workshop plugin's packaging must avoid both.
  - `ACHIEVEMENTS_DISABLED_MODIFIED_GAME` mentions "running a mod". The wiki and fact
    10 say checksum-neutral mods keep achievements, so that check is probably the
    checksum alone. The experiment must confirm this.
- **The checksum is blind to native code.** The checksum only hashes script and map
  files, and `version.dll` lives outside the manifest. A plugin that patches the
  simulation keeps achievements available and passes the multiplayer checksum check;
  in multiplayer it would cause desyncs instead. Paradox forbids distributing cheats.
  Achievement-gaming would be a reputational and policy risk for Anvil and its
  plugin authors, not a Steam technical one. Possible mitigations for the design:
  - a manifest flag such as `affects_gameplay`;
  - a visible plugin-set hash for multiplayer lobbies;
  - an optional refusal to load gameplay plugins when the game is in ironman.
- **Per-version approval is necessary.** Steam auto-updates subscribed items and the
  Workshop cannot freeze an item (fact 4). The planned approval keyed by SHA-256 of
  the entry DLL and manifest (design §9) is the right control. Hashing the whole
  plugin folder would close the gap left by plugins that load more DLLs.

## Open questions and experiments

None of these were run. The lab's startup procedure applies:

- hash the original and the copy before the start;
- set the configuration baseline before and after;
- do not change the Process Lasso rule;
- do not modify saves or existing mods;
- back up and restore `dlc_load.json`.

### E1. Does an `anvil/`-only mod change the checksum or block achievements?

1. Back up `dlc_load.json` and note the current enabled list.
2. Start with no mods enabled. On the main menu, record the 4-character version
   checksum shown with the version string.
3. Create a local mod `mod/anvil-probe.mod` + `mod/anvil-probe/` that contains only:
   - `descriptor.mod`;
   - `anvil/plugins/probe/plugin.toml`;
   - a harmless `anvil/plugins/probe/probe.dll`, for example a build of Anvil's
     example plugin that does nothing. No Anvil `version.dll` is installed, so nothing
     loads it.

   Enable only this mod. Record the checksum.
4. Control: add `common/anvil_probe_control.txt`, an empty `.txt`, to the same mod and
   record the checksum again. It should change, which shows the method can detect a
   change.
5. With the probe mod enabled, without the control file and with Anvil installed and
   loading the probe plugin, open single player:
   - choose a country;
   - tick Ironman;
   - read the achievements tooltip, which lists the `ACHIEVEMENTS_DISABLED_*` reasons.

   Do not start the game, so that no ironman save is created.
6. Compare: the checksum should equal vanilla in steps 2, 3 and 5 and differ in step 4,
   and the tooltip should not list `MODIFIED_GAME`.
7. Restore `dlc_load.json` from the backup, delete the probe mod and its descriptor, and
   check the baseline.

Optional E1b: repeat step 3 with two copies, one with and one without the probe mod. In
the multiplayer lobby, check that both report the same checksum and can join (or rely
on the checksum alone, since the wiki makes it the multiplayer compatibility criterion).

### E2. Does the Paradox launcher uploader or Steam keep `.dll` files?

Needs the user's explicit consent, because it publishes a Workshop item (even a private
one).

1. Use a copy of the E1 probe mod (no checksummed files). Its `probe.dll` should have a
   known SHA-256. Add `README.txt`.
2. Upload it with the launcher's "Upload Mod" button and set visibility to
   **Private** or **Unlisted** on the Workshop page right away.
3. Download it fresh, either with a second account or with
   `steamcmd +workshop_download_item 236850 <id>` into a clean folder. Compare the file
   list and the hashes with the source.
4. Note any launcher or Steam warning, any antivirus reaction, and whether the item
   page shows a moderation notice.
5. Delete the Workshop item (the user does this) and unsubscribe.

### E3. Questions this note could not settle

- The text of the Paradox forum "Rules for user-made mods" (fact 6). A browser read by
  the user, or a Paradox support ticket asking whether "a Workshop mod for EU4 may
  contain a native DLL loaded by a third-party loader", would settle it.
- Whether Valve scans Workshop uploads automatically; nothing official found.
- Whether Paradox Mods (the non-Steam platform) hosts EU4 at all and has its own
  file-type rules (the games list did not render).
