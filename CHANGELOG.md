# Changelog

## [1.25.0](https://github.com/happy-dwarf-team/anvil/compare/v1.24.0...v1.25.0) (2026-11-19)


### Features

* add anvil-model-reload, a modders' window that reloads saved model files in the running game ([#482](https://github.com/happy-dwarf-team/anvil/issues/482)) ([bdde30f](https://github.com/happy-dwarf-team/anvil/commit/bdde30ff965ac07a8447d7d2053a5edea1944f75)), closes [#470](https://github.com/happy-dwarf-team/anvil/issues/470)
* anvil-model-reload creates cities, ports and flags on the map again after reload assets ([#489](https://github.com/happy-dwarf-team/anvil/issues/489)) ([051723d](https://github.com/happy-dwarf-team/anvil/commit/051723d72d51586fea47807308e66463080a680a))
* anvil-model-reload creates envoys, trade goods and ambient objects again, and leaves out damaged files ([#501](https://github.com/happy-dwarf-team/anvil/issues/501)) ([92eae52](https://github.com/happy-dwarf-team/anvil/commit/92eae5270c93f7cbc8952b3ee118d7ec452b4180))
* anvil-model-reload creates the units on the map again after reload assets ([#485](https://github.com/happy-dwarf-team/anvil/issues/485)) ([881b5b8](https://github.com/happy-dwarf-team/anvil/commit/881b5b8701517531310978704c779b075de07533))
* **api:** add anvil-game.camera, the map camera's view and an arrow-key pan ([#447](https://github.com/happy-dwarf-team/anvil/issues/447)) ([ba8698f](https://github.com/happy-dwarf-team/anvil/commit/ba8698f3ec70ea1a0b906a9749d8b2e1f4b7c411))
* **api:** province triggered modifier keys in anvil-game.world ([#448](https://github.com/happy-dwarf-team/anvil/issues/448)) ([1cacfa3](https://github.com/happy-dwarf-team/anvil/commit/1cacfa3aabe96c89d4fce409277e0fda68bab0f2))
* **api:** the regions a fleet may explore in anvil-game.world ([#443](https://github.com/happy-dwarf-team/anvil/issues/443)) ([707eaea](https://github.com/happy-dwarf-team/anvil/commit/707eaea5555d2b111e0c39fab224094167e7d127))
* **api:** typed function symbols for anvil-sdk from anvil gen --lang rust-sdk, and a safe Host::hook_symbol ([#474](https://github.com/happy-dwarf-team/anvil/issues/474)) ([7e55796](https://github.com/happy-dwarf-team/anvil/commit/7e557962371694c13de3b6f9c1c5fd79688fea76))
* **core:** name the owners of script in crash reports ([#442](https://github.com/happy-dwarf-team/anvil/issues/442)) ([ae65c0d](https://github.com/happy-dwarf-team/anvil/commit/ae65c0d130f42951c2f0faf6fc900f3d71c1682c))


### Bug Fixes

* **anvil-game:** refuse a move to a province the map does not have ([#452](https://github.com/happy-dwarf-team/anvil/issues/452)) ([39d7637](https://github.com/happy-dwarf-team/anvil/commit/39d76375ebbfbc009165aea3f37195dd0148f463))
* **anvil-game:** refuse an unsized unit, reach the map without the view symbols, find duplicate flags in one pass ([#473](https://github.com/happy-dwarf-team/anvil/issues/473)) ([f5e4503](https://github.com/happy-dwarf-team/anvil/commit/f5e4503a89683eb34c63784c2564a599e2333d6d))
* **anvil-game:** the world and commands are closed while the old session ends, and non-ASCII flag and variable names round-trip ([#499](https://github.com/happy-dwarf-team/anvil/issues/499)) ([2d0e432](https://github.com/happy-dwarf-team/anvil/commit/2d0e432d672a7f742838123fd4f24a9d358a4799))
* **cli:** let port --out name a file in the current folder ([#523](https://github.com/happy-dwarf-team/anvil/issues/523)) ([9d2e43f](https://github.com/happy-dwarf-team/anvil/commit/9d2e43fb10e1d3f14f45bc5f9ee4e3aec590b8c9))
* **cli:** write files in one step, read a malformed build within its bytes, and refuse names C++ cannot hold ([#492](https://github.com/happy-dwarf-team/anvil/issues/492)) ([bab24d7](https://github.com/happy-dwarf-team/anvil/commit/bab24d7516274a49b517edc63b989c4341735d12))
* **core:** a bad pointer in a change request no longer leaves the registry locked ([#512](https://github.com/happy-dwarf-team/anvil/issues/512)) ([34f8e41](https://github.com/happy-dwarf-team/anvil/commit/34f8e418583f83d34d3aa183f1f8cd1fb656232a))
* **core:** a dependency cycle skips only its members ([#509](https://github.com/happy-dwarf-team/anvil/issues/509)) ([aae2941](https://github.com/happy-dwarf-team/anvil/commit/aae294163de3161ae277c31ce373a4163d8c2c10))
* **core:** a failed pointer swap leaves the plugin's previous cell as it was ([#495](https://github.com/happy-dwarf-team/anvil/issues/495)) ([e947988](https://github.com/happy-dwarf-team/anvil/commit/e94798846d5a9d2cdc8c381f6adc094f40d5b361))
* **core:** a file name that is not valid Unicode refuses its plugin, not the start ([#526](https://github.com/happy-dwarf-team/anvil/issues/526)) ([9d75175](https://github.com/happy-dwarf-team/anvil/commit/9d75175fb22613d6b2433999248872e6526d6e6a))
* **core:** a hook needs code even when an export declares no memory ([#528](https://github.com/happy-dwarf-team/anvil/issues/528)) ([f5e23dd](https://github.com/happy-dwarf-team/anvil/commit/f5e23dd1d36289a65a8a8a85a3529b5c287d42ec))
* **core:** a plugin whose detour only passed the call on is not "on the stack" of a crash ([#481](https://github.com/happy-dwarf-team/anvil/issues/481)) ([a34c5a1](https://github.com/happy-dwarf-team/anvil/commit/a34c5a1cc4b4906e7541182cfa0211c576250237))
* **core:** a removal that cannot restore the memory keeps the change ([#519](https://github.com/happy-dwarf-team/anvil/issues/519)) ([12393c6](https://github.com/happy-dwarf-team/anvil/commit/12393c67338b642d195f53f6636ba3c79dc654e3))
* **core:** free the plugin inventory's parsed symbols once the plugins have theirs ([#462](https://github.com/happy-dwarf-team/anvil/issues/462)) ([ce387bc](https://github.com/happy-dwarf-team/anvil/commit/ce387bc196933d6f80e9725cc887004fd57a0c77)), closes [#461](https://github.com/happy-dwarf-team/anvil/issues/461)
* **core:** keep a player's settings file that does not parse ([#514](https://github.com/happy-dwarf-team/anvil/issues/514)) ([32b2c10](https://github.com/happy-dwarf-team/anvil/commit/32b2c10ef625962d81dff0e5dae6c248e1b64ad6))
* **core:** keep crash reports for broken frame pointers and caught stack overflows ([#455](https://github.com/happy-dwarf-team/anvil/issues/455)) ([522e374](https://github.com/happy-dwarf-team/anvil/commit/522e3745de2de6b7fd859d290095273913d6d905))
* **core:** let no C++ exception leave the core's C API ([#460](https://github.com/happy-dwarf-team/anvil/issues/460)) ([24a600a](https://github.com/happy-dwarf-team/anvil/commit/24a600a6642a3d9957f03c709e0ca8c669abac95)), closes [#457](https://github.com/happy-dwarf-team/anvil/issues/457)
* **core:** read plugin file names as UTF-8 so one plugin cannot end the start-up ([#451](https://github.com/happy-dwarf-team/anvil/issues/451)) ([c13fd60](https://github.com/happy-dwarf-team/anvil/commit/c13fd6087b5dff401218bd91e2b7aaa13db621d8))
* **core:** refuse changes from a plugin whose load failed ([#454](https://github.com/happy-dwarf-team/anvil/issues/454)) ([ba9890d](https://github.com/happy-dwarf-team/anvil/commit/ba9890df556b45ddfc553ab903c73a4e8b257fa4))
* **core:** refuse hooks whose stolen bytes branch back into themselves ([#453](https://github.com/happy-dwarf-team/anvil/issues/453)) ([f377c7e](https://github.com/happy-dwarf-team/anvil/commit/f377c7e600338a79485619f71da5f0d7516b2ee6))
* **core:** restore each page's own protection after a write across pages ([#515](https://github.com/happy-dwarf-team/anvil/issues/515)) ([7a76c49](https://github.com/happy-dwarf-team/anvil/commit/7a76c49e518052e3b93d37a1e39cc76ad1c77ef8))
* **core:** send anvil.suspended and anvil.resumed after the gameplay policy's lock ([#513](https://github.com/happy-dwarf-team/anvil/issues/513)) ([27be6b7](https://github.com/happy-dwarf-team/anvil/commit/27be6b7f7946fd9080b2412af70621601e6b8130))
* **core:** version ranges below 1.0, partial bounds and an upper bound's prereleases ([#518](https://github.com/happy-dwarf-team/anvil/issues/518)) ([3617288](https://github.com/happy-dwarf-team/anvil/commit/3617288bf5a10c1f6f260c483eb36cbd8341c94d))
* **core:** write anvil.log as UTF-8 and read a cut-off crash report only within its bytes ([#471](https://github.com/happy-dwarf-team/anvil/issues/471)) ([e7736d9](https://github.com/happy-dwarf-team/anvil/commit/e7736d94faa7bf516ddd7c5b7637f736b39d57bf))
* **core:** write the crash report before the plugins' notes ([#445](https://github.com/happy-dwarf-team/anvil/issues/445)) ([3a23a86](https://github.com/happy-dwarf-team/anvil/commit/3a23a864ee40a7cdcd15cf8c3122f366323fd932))
* **core:** write the crash report while the process heap is locked ([#459](https://github.com/happy-dwarf-team/anvil/issues/459)) ([07be0b3](https://github.com/happy-dwarf-team/anvil/commit/07be0b3022fc2c22b9c27ed63414939b94cb14db))
* follow-ups to [#505](https://github.com/happy-dwarf-team/anvil/issues/505) and [#507](https://github.com/happy-dwarf-team/anvil/issues/507) from a second review ([#508](https://github.com/happy-dwarf-team/anvil/issues/508)) ([22640fd](https://github.com/happy-dwarf-team/anvil/commit/22640fd9bd1b117310253191bb8f00dadfd4653d))
* **game:** the world stays closed for the rest of a frame in which a save was loaded ([#511](https://github.com/happy-dwarf-team/anvil/issues/511)) ([197f163](https://github.com/happy-dwarf-team/anvil/commit/197f16399f5d7ac5d822ac0d6f9b301380135375))
* **inspector:** a name longer than a panel row takes no longer stops the window's refresh ([#490](https://github.com/happy-dwarf-team/anvil/issues/490)) ([8476913](https://github.com/happy-dwarf-team/anvil/commit/8476913c75f5637562b446681fae7888dcf990cc))
* **log-throttle:** a very long event target, file or game file no longer stops the error panel's refresh ([#487](https://github.com/happy-dwarf-team/anvil/issues/487)) ([b792f07](https://github.com/happy-dwarf-team/anvil/commit/b792f07018c684cc771882520a8a3bb1134e7785))
* **log-throttle:** reports go only into their own log, and the panel starts once the plugin is active ([#505](https://github.com/happy-dwarf-team/anvil/issues/505)) ([7cf21ea](https://github.com/happy-dwarf-team/anvil/commit/7cf21ea2295fbf65442f4b20c2e803d85d201000))
* **manager:** a crash report rotated out is no longer selected, a download ends unused after a folder change, a module's default from the entry that declares it ([#484](https://github.com/happy-dwarf-team/anvil/issues/484)) ([2819ee2](https://github.com/happy-dwarf-team/anvil/commit/2819ee2bac4cd68529282ea746a0a12a6404002d))
* **manager:** an eu4.exe that is exiting runs nowhere, so another game closing no longer makes this one read-only ([#480](https://github.com/happy-dwarf-team/anvil/issues/480)) ([121ac21](https://github.com/happy-dwarf-team/anvil/commit/121ac2165405f1aea944e2c71f7687abb932928c))
* **manager:** check the game at Revoke, and keep zips out of a benchmark series ([#525](https://github.com/happy-dwarf-team/anvil/issues/525)) ([e3b7f60](https://github.com/happy-dwarf-team/anvil/commit/e3b7f607b05a67d91e1059b728b1b1eb6ae3156c))
* **manager:** closing ends running transfers, so the process does not stay on without a window ([#491](https://github.com/happy-dwarf-team/anvil/issues/491)) ([acb63ee](https://github.com/happy-dwarf-team/anvil/commit/acb63ee53b348d2c69cc4a818ae305328c945271))
* **manager:** cut the receiver's message at a whole character ([#529](https://github.com/happy-dwarf-team/anvil/issues/529)) ([5e0097c](https://github.com/happy-dwarf-team/anvil/commit/5e0097c970c619530555c35e629cd4eb6ceda09d))
* **manager:** move back a plugin folder moved aside when the next move fails ([#521](https://github.com/happy-dwarf-team/anvil/issues/521)) ([edea82f](https://github.com/happy-dwarf-team/anvil/commit/edea82fde1d597b5bc4f0a25646cb77dc273fd8f))
* **manager:** no empty optional in the plan dialog, benchmark checks again before a series, no approval under a running game ([#483](https://github.com/happy-dwarf-team/anvil/issues/483)) ([d730146](https://github.com/happy-dwarf-team/anvil/commit/d73014654cda08d419dfb4e616c26ab55296138a))
* **manager:** remove a plugin folder in one move, and not while the game runs ([#522](https://github.com/happy-dwarf-team/anvil/issues/522)) ([6b1c077](https://github.com/happy-dwarf-team/anvil/commit/6b1c077f2840a6920e938da0ec3824f88aea5252))
* **manager:** Repair removes a junctioned plugin folder's link, not the files behind it, and keeps a file named in another case ([eeb18d9](https://github.com/happy-dwarf-team/anvil/commit/eeb18d980f2d462a597518ad277f7151789f16f6))
* **manager:** repair removes a junctioned plugin folder's link, not the files behind it, and keeps a file named in another case ([#500](https://github.com/happy-dwarf-team/anvil/issues/500)) ([eeb18d9](https://github.com/happy-dwarf-team/anvil/commit/eeb18d980f2d462a597518ad277f7151789f16f6))
* **overlay:** a key recording ends with the panel, a notice's Disable keeps its plugin id, Shift+Insert and Alt+F4 with a field focused ([#486](https://github.com/happy-dwarf-team/anvil/issues/486)) ([7733330](https://github.com/happy-dwarf-team/anvil/commit/7733330acbd7a313867b599b71202bb53bcea974))
* **overlay:** end a frame that an exception left half drawn, so no pushed style stays in force ([#478](https://github.com/happy-dwarf-team/anvil/issues/478)) ([f951be7](https://github.com/happy-dwarf-team/anvil/commit/f951be7c3e26d20b1fd4a562f4a8a743fa908a86))
* **overlay:** let go of ImGui's device before the game creates another ([#506](https://github.com/happy-dwarf-team/anvil/issues/506)) ([e331f72](https://github.com/happy-dwarf-team/anvil/commit/e331f723d20c3cc7809ab00dcbb848ba5f0161b7))
* **overlay:** no mouse capture, drag or hover left over after the overlay hides or the cursor leaves ([#488](https://github.com/happy-dwarf-team/anvil/issues/488)) ([28a4bbb](https://github.com/happy-dwarf-team/anvil/commit/28a4bbb33413da1ffb6bc869b291d3f6d1a86d79))
* **plugins:** let no exception leave the panels' clicks, anvil-script's C entries and two thread bodies ([#464](https://github.com/happy-dwarf-team/anvil/issues/464)) ([0570c51](https://github.com/happy-dwarf-team/anvil/commit/0570c511c7e8ffae3bd70a71c77df3c8b5a659cc))
* **plugins:** run each queued work item once, keep the tally's lists in step, log profiler paths as UTF-8 ([#463](https://github.com/happy-dwarf-team/anvil/issues/463)) ([02a8311](https://github.com/happy-dwarf-team/anvil/commit/02a831122b6f716cad7924a3d8e94267dd472677))
* **plugins:** start helper threads inside try ([#456](https://github.com/happy-dwarf-team/anvil/issues/456)) ([5414f1a](https://github.com/happy-dwarf-team/anvil/commit/5414f1af4228320ca4d1975b010f51fe6f3f2a70))
* **proxy:** look up the real version.dll exports without a static's init guard ([#477](https://github.com/happy-dwarf-team/anvil/issues/477)) ([5a833d1](https://github.com/happy-dwarf-team/anvil/commit/5a833d1ca1c47e862dabf056e9bb63616c3538c1))
* refuse a release link with dot segments or percent escapes ([#517](https://github.com/happy-dwarf-team/anvil/issues/517)) ([bec6880](https://github.com/happy-dwarf-team/anvil/commit/bec6880bb18a78ae30b9d5356ca8b48f24739787))
* refuse device names as plugin ids, and zip names that NTFS takes for one file ([#520](https://github.com/happy-dwarf-team/anvil/issues/520)) ([4ec1d81](https://github.com/happy-dwarf-team/anvil/commit/4ec1d819bffc74499dc207be59c1a96abde497e8))
* **script-profiler:** write the CSV to the game's own user folder, also with userdir.txt ([#502](https://github.com/happy-dwarf-team/anvil/issues/502)) ([064dfaf](https://github.com/happy-dwarf-team/anvil/commit/064dfafbd36e19289d3405c54ccd52e850791d7f))
* **script:** a plugin's effect or day-end callback that throws no longer ends the game ([#510](https://github.com/happy-dwarf-team/anvil/issues/510)) ([1299b7a](https://github.com/happy-dwarf-team/anvil/commit/1299b7ac12c47afe06c3427a293ffc6c221b3392))
* **sdk:** a C++ group whose changes are dropped frees the hooks it filled, so the profiler starts again after a refused Start ([#504](https://github.com/happy-dwarf-team/anvil/issues/504)) ([6e303a0](https://github.com/happy-dwarf-team/anvil/commit/6e303a063d7b7b56b44ff924cb51fb80fa6c54a2))
* **sdk:** a detour entered as soon as its first change is enabled has next ([#507](https://github.com/happy-dwarf-team/anvil/issues/507)) ([34e916a](https://github.com/happy-dwarf-team/anvil/commit/34e916a64a84f73941e5b562fac03c608784c9a8))
* **sdk:** a failed request on a reused C++ FunctionHook keeps next, and one request claims a hook at a time ([#497](https://github.com/happy-dwarf-team/anvil/issues/497)) ([e679736](https://github.com/happy-dwarf-team/anvil/commit/e6797360866d5f62e16ffbd5a371b9dfa75ec259))
* **sdk:** a failed request on a reused FunctionHook keeps next, and one request claims a hook at a time ([#479](https://github.com/happy-dwarf-team/anvil/issues/479)) ([7098c58](https://github.com/happy-dwarf-team/anvil/commit/7098c58d97b305644af2552beaaf439578a8414e))
* **sdk:** a Rust group whose changes are dropped frees the hooks it filled ([#503](https://github.com/happy-dwarf-team/anvil/issues/503)) ([2e66997](https://github.com/happy-dwarf-team/anvil/commit/2e669979ca38203d7b7407ed42051bdd555fdcf4))
* **sdk:** log a panic out of Host::group that no adapter catches ([#475](https://github.com/happy-dwarf-team/anvil/issues/475)) ([28e7d0c](https://github.com/happy-dwarf-team/anvil/commit/28e7d0cb62174ca3a7ee6e1c805e72658d1ec5b1))
* **sdk:** make Host::hook and publish_raw unsafe, read service tables only as far as their size ([#467](https://github.com/happy-dwarf-team/anvil/issues/467)) ([faea1be](https://github.com/happy-dwarf-team/anvil/commit/faea1be0ec5fe06d9fb121470d7f0ef8ed0e1c6c))
* **sdk:** read anvil-script's call and day only as far as their size ([#476](https://github.com/happy-dwarf-team/anvil/issues/476)) ([c726e45](https://github.com/happy-dwarf-team/anvil/commit/c726e45f744c61940611263b6e005cf3b3c12c27))
* **sdk:** read the game's SHA-256 atomically in Host::game ([#465](https://github.com/happy-dwarf-team/anvil/issues/465)) ([dd6a762](https://github.com/happy-dwarf-team/anvil/commit/dd6a7620e746c4e245f0271f813a2a4923a89a3e))

## [1.24.0](https://github.com/happy-dwarf-team/anvil/compare/v1.23.0...v1.24.0) (2026-11-15)


### Features

* **api:** show a plugin map mode through anvil-map.modes ([#404](https://github.com/happy-dwarf-team/anvil/issues/404)) ([0601961](https://github.com/happy-dwarf-team/anvil/commit/06019619e7da3ce6c73943eb3cd0e1b46014f632)), closes [#380](https://github.com/happy-dwarf-team/anvil/issues/380)
* **api:** the game speed and the pause in anvil-game.game ([#400](https://github.com/happy-dwarf-team/anvil/issues/400)) ([c82f43a](https://github.com/happy-dwarf-team/anvil/commit/c82f43aa36ef40016552e425fcd827024ef2498f)), closes [#378](https://github.com/happy-dwarf-team/anvil/issues/378)
* **api:** the game's write folder in anvil-game.game ([#405](https://github.com/happy-dwarf-team/anvil/issues/405)) ([3090b01](https://github.com/happy-dwarf-team/anvil/commit/3090b01a5bc055eb38606fcd0b7b0b7f7fe6e6ce)), closes [#381](https://github.com/happy-dwarf-team/anvil/issues/381)
* **api:** the selected province and the province under a point in anvil-game.world ([#408](https://github.com/happy-dwarf-team/anvil/issues/408)) ([4fedda1](https://github.com/happy-dwarf-team/anvil/commit/4fedda191aa087cbcb1b678f4afeeaf755eeefca)), closes [#379](https://github.com/happy-dwarf-team/anvil/issues/379)
* **cli:** anvil check --game shows the next start's plan, its safe mode and the writes it makes first; the JSON report gains next_start ([048da8b](https://github.com/happy-dwarf-team/anvil/commit/048da8b74007464476036fc100c2fa7f5937bae1))
* **manager:** the Plugins tab shows each plugin as the next start will load it, with a banner when the next start is in safe mode and one for what it writes first, such as retired settings that become module switches ([048da8b](https://github.com/happy-dwarf-team/anvil/commit/048da8b74007464476036fc100c2fa7f5937bae1))
* the overlay's Plugins tab shows what changes from the next start apart from what runs in this game ([048da8b](https://github.com/happy-dwarf-team/anvil/commit/048da8b74007464476036fc100c2fa7f5937bae1))
* **ui:** module rows that open, and a toggle for what acts at once ([#391](https://github.com/happy-dwarf-team/anvil/issues/391)) ([f33462f](https://github.com/happy-dwarf-team/anvil/commit/f33462fe18d693fe8c5bb148d0221e17fde4fbd0))


### Bug Fixes

* **core:** a plugin from a mod whose approved copy cannot be made now fails with the reason, with the plugins that need it, instead of being skipped ([048da8b](https://github.com/happy-dwarf-team/anvil/commit/048da8b74007464476036fc100c2fa7f5937bae1))
* **core:** a retired value switches its module at this start when anvil.toml cannot be written ([#411](https://github.com/happy-dwarf-team/anvil/issues/411)) ([10eaeac](https://github.com/happy-dwarf-team/anvil/commit/10eaeaca33a2eeecf3160112b778ac99b98a77f3))
* **core:** a retired value that waits for its move survives a settings save ([#402](https://github.com/happy-dwarf-team/anvil/issues/402)) ([3f620c5](https://github.com/happy-dwarf-team/anvil/commit/3f620c551d3608fb4424712748863649bb17bd65))
* **core:** an environment variable of a retired setting is a warning, not silence ([#397](https://github.com/happy-dwarf-team/anvil/issues/397)) ([36645ed](https://github.com/happy-dwarf-team/anvil/commit/36645edeed5e76400f44caea0434ff7b066d3980))
* **core:** keep a module switch made after a retired value could not leave the player's file ([#436](https://github.com/happy-dwarf-team/anvil/issues/436)) ([da66f5d](https://github.com/happy-dwarf-team/anvil/commit/da66f5dfcf94c5e0999d7c713f6fabb425d6d140))
* **core:** let off win when two retired settings of one module disagree ([#437](https://github.com/happy-dwarf-team/anvil/issues/437)) ([f5c8c03](https://github.com/happy-dwarf-team/anvil/commit/f5c8c038276fade06da6c4379c56c6b30c2f37d6))
* **core:** record retired values a start moved but could not remove from the player's file ([da66f5d](https://github.com/happy-dwarf-team/anvil/commit/da66f5dfcf94c5e0999d7c713f6fabb425d6d140))
* **core:** refuse to disable the overlay or what it needs at the next start ([#430](https://github.com/happy-dwarf-team/anvil/issues/430)) ([e55a29d](https://github.com/happy-dwarf-team/anvil/commit/e55a29d09c5e6bba69cbe9bf2b419c37d511c6b1))
* **core:** retired settings move from one folder of an id, and none when one loads ([#410](https://github.com/happy-dwarf-team/anvil/issues/410)) ([9d7b6c0](https://github.com/happy-dwarf-team/anvil/commit/9d7b6c01b729626c62bfb713c7ab2da2a67b605f))
* **core:** retired settings write no file at a benchmark start, nor for a duplicate id ([#396](https://github.com/happy-dwarf-team/anvil/issues/396)) ([3bc9014](https://github.com/happy-dwarf-team/anvil/commit/3bc901456996d134a37b6be59008ab1d7ab39711))
* **core:** the player's settings file is flushed before it replaces the old one ([#412](https://github.com/happy-dwarf-team/anvil/issues/412)) ([7d0c412](https://github.com/happy-dwarf-team/anvil/commit/7d0c41213a939626c414f57ba05e0c4ca1cac093))
* **game:** unit_at answers not found for a pick that is not a unit ([#417](https://github.com/happy-dwarf-team/anvil/issues/417)) ([0470304](https://github.com/happy-dwarf-team/anvil/commit/04703047c85b2f9d6e19984f75360c9b38a5623a))
* **game:** user_dir() gives a long write folder whole and stays valid at exit ([#409](https://github.com/happy-dwarf-team/anvil/issues/409)) ([3ddffac](https://github.com/happy-dwarf-team/anvil/commit/3ddffac1f18e91ed4e12eae8b760acf60a513621))
* **game:** user_dir() keeps no NULs when the folder got shorter ([#421](https://github.com/happy-dwarf-team/anvil/issues/421)) ([9c97ab3](https://github.com/happy-dwarf-team/anvil/commit/9c97ab3c1c6336e1b5caa79383a1f92ea80d9367))
* **log-throttle:** Find skips a subfolder that a later mod replaces ([196baca](https://github.com/happy-dwarf-team/anvil/commit/196bacac1b7063ebf6adadaf17f7826483fd52c0))
* **log-throttle:** keep a replaced folder's subfolders in Find ([#431](https://github.com/happy-dwarf-team/anvil/issues/431)) ([30d9b4f](https://github.com/happy-dwarf-team/anvil/commit/30d9b4f5a638fe01a306ec1a65090271fb93372d))
* **log-throttle:** the errors panel's Find skips a subfolder that a later mod replaces ([#399](https://github.com/happy-dwarf-team/anvil/issues/399)) ([196baca](https://github.com/happy-dwarf-team/anvil/commit/196bacac1b7063ebf6adadaf17f7826483fd52c0))
* **manager:** before the first start after an update that retires a setting, the Plugins tab shows the module as the start will switch it and no longer calls the retired setting unknown ([048da8b](https://github.com/happy-dwarf-team/anvil/commit/048da8b74007464476036fc100c2fa7f5937bae1))
* **manager:** keep --lang to the Manager, not the game it starts ([#432](https://github.com/happy-dwarf-team/anvil/issues/432)) ([fce73c9](https://github.com/happy-dwarf-team/anvil/commit/fce73c9b17711531aef79cbab15b3e673a35eccc))
* **manager:** read a crash report again when it was not whole ([#419](https://github.com/happy-dwarf-team/anvil/issues/419)) ([894db06](https://github.com/happy-dwarf-team/anvil/commit/894db06ee16cdb7056e38429f30958ccaba7872f))
* **manager:** the Undo of the Plugins page writes back the last start's value ([#393](https://github.com/happy-dwarf-team/anvil/issues/393)) ([34f8401](https://github.com/happy-dwarf-team/anvil/commit/34f84013d69f5de42fc6b35f0e5840933ea0d702))
* **manager:** turn Play off when Steam would start another folder ([#438](https://github.com/happy-dwarf-team/anvil/issues/438)) ([d3a7ffb](https://github.com/happy-dwarf-team/anvil/commit/d3a7ffbeda62ed49fc14afe3f4b3cc69b3254a76))
* **manager:** Undo writes back the last start's value of a setting ([34f8401](https://github.com/happy-dwarf-team/anvil/commit/34f84013d69f5de42fc6b35f0e5840933ea0d702))
* **map:** a show request never outlives the session it was asked in ([#407](https://github.com/happy-dwarf-team/anvil/issues/407)) ([a95ed59](https://github.com/happy-dwarf-team/anvil/commit/a95ed59b5ea94f53e265d84e35ce9c3089f4a8f8))
* **map:** set_colors drops a buffer of a session that ended during the call ([#416](https://github.com/happy-dwarf-team/anvil/issues/416)) ([840c413](https://github.com/happy-dwarf-team/anvil/commit/840c4134106225f74717b4b0b9d07284f62bf788))
* **overlay:** open a plugin's repository only at an https address ([#418](https://github.com/happy-dwarf-team/anvil/issues/418)) ([c7b0b71](https://github.com/happy-dwarf-team/anvil/commit/c7b0b71b1323bf7a532a7ecf8d2cb5215cbe746d))
* **overlay:** Open its repository opens the mod plugin's repository ([c7b0b71](https://github.com/happy-dwarf-team/anvil/commit/c7b0b71b1323bf7a532a7ecf8d2cb5215cbe746d))
* **overlay:** stop a dependency chain at its first refusal ([#428](https://github.com/happy-dwarf-team/anvil/issues/428)) ([95eac82](https://github.com/happy-dwarf-team/anvil/commit/95eac825d477f1d7298e46accb5f92474ce3bbc8))
* **ui:** a module row stays as it opened, and counts its locked changes ([#415](https://github.com/happy-dwarf-team/anvil/issues/415)) ([9e18a66](https://github.com/happy-dwarf-team/anvil/commit/9e18a66a8601e90844d23b7af3ee9fcc6391ee35))
* **ui:** a module that is off names the opt-ins it keeps ([#435](https://github.com/happy-dwarf-team/anvil/issues/435)) ([c370330](https://github.com/happy-dwarf-team/anvil/commit/c370330dcec36cf2a41cfe5f5f583f873115bb32))
* **ui:** keep a changed yes/no setting's note beside its long label ([#394](https://github.com/happy-dwarf-team/anvil/issues/394)) ([56433a4](https://github.com/happy-dwarf-team/anvil/commit/56433a48a00d647695a93536fd57d945fd5e7be3))
* **ui:** keep a notice's title off its close button and the plugin switch off its buttons ([#433](https://github.com/happy-dwarf-team/anvil/issues/433)) ([61a9f6e](https://github.com/happy-dwarf-team/anvil/commit/61a9f6e9d84477de96fde4e252fe086072a873b4))
* **ui:** resets and Undo leave locked settings alone, and Undo shows only when it can act ([#395](https://github.com/happy-dwarf-team/anvil/issues/395)) ([75fdedf](https://github.com/happy-dwarf-team/anvil/commit/75fdedf307780d81db601d8121f8ec6cb34af256))
* **ui:** settings of a second entry of an id are read-only, its approval counts ([#429](https://github.com/happy-dwarf-team/anvil/issues/429)) ([44903ec](https://github.com/happy-dwarf-team/anvil/commit/44903ec3d43c382bb046a613a9c70f06d759f316))
* **ui:** show a switch's refusal over its track, not only its word ([#424](https://github.com/happy-dwarf-team/anvil/issues/424)) ([ef47e86](https://github.com/happy-dwarf-team/anvil/commit/ef47e8644cc75e41b600fe857b2c36dff5d02070))
* **ui:** two entries of one id in the Plugins list each open their own detail ([#401](https://github.com/happy-dwarf-team/anvil/issues/401)) ([dc63436](https://github.com/happy-dwarf-team/anvil/commit/dc63436ce910ace91a4b2a0984cdf9d742a44fa0))

## [1.23.0](https://github.com/happy-dwarf-team/anvil/compare/v1.22.0...v1.23.0) (2026-11-15)

### Features

* Anvil Manager and the in-game overlay share a new warm dark look, with an ember accent on the selected tab and the main button of each view, and every text meets a 4.5:1 contrast, on every page of the Manager and in the overlay's badge, notices, crash report and log. ([#368](https://github.com/happy-dwarf-team/anvil/issues/368)) ([a30ad31](https://github.com/happy-dwarf-team/anvil/commit/a30ad3123f3d938fdbaad88653172c46bd85da06)) ([#384](https://github.com/happy-dwarf-team/anvil/issues/384)) ([9fa2f42](https://github.com/happy-dwarf-team/anvil/commit/9fa2f424cd603cc66902b730a1425d091517c8cc)) ([#387](https://github.com/happy-dwarf-team/anvil/issues/387)) ([e23fdea](https://github.com/happy-dwarf-team/anvil/commit/e23fdeac7c496ded4ce053c47c60719a071bf226)) ([#386](https://github.com/happy-dwarf-team/anvil/issues/386)) ([1a85972](https://github.com/happy-dwarf-team/anvil/commit/1a85972625cb56cbcca400b15212e19be92a26a9)) ([#388](https://github.com/happy-dwarf-team/anvil/issues/388)) ([a9f940c](https://github.com/happy-dwarf-team/anvil/commit/a9f940ce0dff6d53ab22b2b9d6342a58e486e5e5))
* Anvil Manager has a new layout: a sidebar with Play EU4 on top and the pages under it, folding to icons in a narrow window, and a dark title bar with the Anvil icon on Windows 11. ([#383](https://github.com/happy-dwarf-team/anvil/issues/383)) ([b0af31e](https://github.com/happy-dwarf-team/anvil/commit/b0af31e8e7dc6d9cecb8f6967beba78db5f1ad1a))
* The Manager's Mods and Settings tabs become one Plugins page: each plugin with its modules, settings and dependencies, a search and filters, and a mark on changes that wait for the next start. Its new texts in other languages are drafts, not yet checked by native speakers. ([#375](https://github.com/happy-dwarf-team/anvil/issues/375)) ([f943441](https://github.com/happy-dwarf-team/anvil/commit/f9434418e7e7c6d8ad099ab78adc4f6baf989c76)) ([#371](https://github.com/happy-dwarf-team/anvil/issues/371)) ([67c0af1](https://github.com/happy-dwarf-team/anvil/commit/67c0af1c842f23726ffc8f3b38a6ae6e33cb3e7f))
* The overlay's Status, Settings and Mods tabs are now one Plugins tab, as in the Manager. From the game you can turn plugins and modules on or off and allow them in ironman or multiplayer for the next start, also for a plugin that is off now. ([#382](https://github.com/happy-dwarf-team/anvil/issues/382)) ([79fc8e6](https://github.com/happy-dwarf-team/anvil/commit/79fc8e6c9806f6e8e098051ddad5455fd008bbb8)) ([#389](https://github.com/happy-dwarf-team/anvil/issues/389)) ([735e5ab](https://github.com/happy-dwarf-team/anvil/commit/735e5abec343c20789bbb1853400ee9c94bf2eca)) ([#370](https://github.com/happy-dwarf-team/anvil/issues/370)) ([f130ef1](https://github.com/happy-dwarf-team/anvil/commit/f130ef159f725727a7b3e0fc3006c906146eede1)) ([#377](https://github.com/happy-dwarf-team/anvil/issues/377)) ([e06963c](https://github.com/happy-dwarf-team/anvil/commit/e06963cf86690f21d936b0e3d2b8680f35c1b8d5)) ([#369](https://github.com/happy-dwarf-team/anvil/issues/369)) ([1d935d1](https://github.com/happy-dwarf-team/anvil/commit/1d935d19b1837e63438ba50f6856e90761fb3de1))
* A Restart game button in the Anvil panel restarts EU4 with the same mods and launch options, after a confirmation. A campaign first closes as at Exit to Menu (ironman saves), and it is off in multiplayer. Plugins get `restart` in `anvil-game.game` (Rust: `anvil-sdk` 0.1.11). ([#362](https://github.com/happy-dwarf-team/anvil/issues/362)) ([fe27e12](https://github.com/happy-dwarf-team/anvil/commit/fe27e1296defc491027054dfe32c2890b940320a))
* Settings that change the game now count in the multiplayer code and are locked during a multiplayer game, and the overlay shows the code a game started now would have, so players can compare it in the lobby. ([#373](https://github.com/happy-dwarf-team/anvil/issues/373)) ([d4db69b](https://github.com/happy-dwarf-team/anvil/commit/d4db69bad998a247da7273420f97726f7d30e54b))
* For modders: the script errors panel shows the game's own Context of an error (the event, option or mission that was running) and a Find that opens that event's definition. ([#376](https://github.com/happy-dwarf-team/anvil/issues/376)) ([8269a3b](https://github.com/happy-dwarf-team/anvil/commit/8269a3b08a2775c6a91f752e9bca81bd35b2ef63))
* For plugin authors: a setting takes `module`, `advanced` and `affects_gameplay`, `[[modules]]` a `group`, and `[retired.<name>]` moves a dropped setting into a module's switch or opt-ins, so players keep their choice. They need `anvil = ">=1.23.0"`; `anvil check` warns. ([#367](https://github.com/happy-dwarf-team/anvil/issues/367)) ([7bd1ed3](https://github.com/happy-dwarf-team/anvil/commit/7bd1ed30b21931eb56bbf47b144f203535c64ea0)) ([#374](https://github.com/happy-dwarf-team/anvil/issues/374)) ([3ea0b05](https://github.com/happy-dwarf-team/anvil/commit/3ea0b05cac3ea929716395581730b11686848574))
* For plugin authors: `player_tags` in `anvil-game.world` gives the player's own tag, the observed tag and whether the player observes (Rust: `anvil-sdk` 0.1.11). ([#364](https://github.com/happy-dwarf-team/anvil/issues/364)) ([ef610c3](https://github.com/happy-dwarf-team/anvil/commit/ef610c3c4981cd1ba565a0e3dbe3babc55a1ee58))

### Bug Fixes

* Installing a plugin zip whose modules change the game now says so before you confirm. ([#372](https://github.com/happy-dwarf-team/anvil/issues/372)) ([8cc18c0](https://github.com/happy-dwarf-team/anvil/commit/8cc18c03cefbaadec0dc97f6a31bf05d20312402))
* A crash inside a module's change is named after the right plugin, and a module of a plugin that failed before loading shows as failed instead of pending. ([#359](https://github.com/happy-dwarf-team/anvil/issues/359)) ([f505a0d](https://github.com/happy-dwarf-team/anvil/commit/f505a0d914e41270f7556a56ea39440ff959adc7)) ([#365](https://github.com/happy-dwarf-team/anvil/issues/365)) ([1340dc2](https://github.com/happy-dwarf-team/anvil/commit/1340dc22cffcda184dfb2f8375eeb735555b85ea))

## [1.22.0](https://github.com/happy-dwarf-team/anvil/compare/v1.21.0...v1.22.0) (2026-11-13)

### Features

* Plugins can split their features into modules, each with its own switch: the Manager's Plugins tab and the overlay's Status tab show them, ironman and multiplayer are allowed module by module, and approving a plugin from a mod lists what each module does to the game. ([0103a0c](https://github.com/happy-dwarf-team/anvil/commit/0103a0ca1e4592a50487def8bde2416e5226dcf3)) ([95ae765](https://github.com/happy-dwarf-team/anvil/commit/95ae7650401a3f0e85467b909948ca7cf1cc7964)) ([3c1e28d](https://github.com/happy-dwarf-team/anvil/commit/3c1e28dbd812991585eed08d8536636e3d03a2f7))
* **api:** For plugin authors, `[[modules]]` in `plugin.toml` and `host->module` (in Rust through `anvil-sdk` 0.1.10); `anvil check` lists a plugin's modules. ([5f07509](https://github.com/happy-dwarf-team/anvil/commit/5f07509931ec4cac53b60b18655f8123e814d3fc)) ([f9d82f7](https://github.com/happy-dwarf-team/anvil/commit/f9d82f7d39705d46469d6457c61ac7257be20739))
* Modders get a script profiler, the new standard plugin `anvil-script-profiler`: **Start** in its window (Status tab) times the triggers and effects of each event, decision, mission, scripted trigger and church aspect and writes a CSV. It is idle until started and slows the game while it measures. ([#345](https://github.com/happy-dwarf-team/anvil/issues/345)) ([cf7a59d](https://github.com/happy-dwarf-team/anvil/commit/cf7a59df3a1a41a909f5428e9886b75ae8eb0d84)) ([#352](https://github.com/happy-dwarf-team/anvil/issues/352)) ([f46abc7](https://github.com/happy-dwarf-team/anvil/commit/f46abc7967bf82afeec9eb2cab34754183e7fa45))
* **api:** `anvil-game.world` reads diplomacy and the opinion the game shows. ([#344](https://github.com/happy-dwarf-team/anvil/issues/344)) ([cb06407](https://github.com/happy-dwarf-team/anvil/commit/cb06407505b4c3adb7dbb28de5beb101b5f624d5))
* **api:** `anvil-game.commands` version 3 sets the game speed, sends armies and fleets exploring or moving, and adds a map area to an army's siege or rebel hunt (`anvil-sdk` 0.1.10). ([77c924e](https://github.com/happy-dwarf-team/anvil/commit/77c924e44c9d4c28bf49b2b3b6d8791687f5642b)) ([614b31e](https://github.com/happy-dwarf-team/anvil/commit/614b31e48a6aa43fa7ff0291aae5fd03c3f9baae)) ([91e53b0](https://github.com/happy-dwarf-team/anvil/commit/91e53b0ae6523e64947dcdaa1c8af1803667bdec))

### Bug Fixes

* **api:** `anvil::result_name` names `ANVIL_ERROR_DISABLED`, a module the player turned off, instead of "unknown result". ([#356](https://github.com/happy-dwarf-team/anvil/issues/356)) ([a7c152d](https://github.com/happy-dwarf-team/anvil/commit/a7c152dccb7c500f333066290c7b14250271ca69))

### Build

* Plugin authors on Linux can build Anvil and their plugins with clang-cl and run the tests under Wine; the SDK carries the toolchain file. Windows builds are unchanged. ([#355](https://github.com/happy-dwarf-team/anvil/issues/355)) ([1dd8d91](https://github.com/happy-dwarf-team/anvil/commit/1dd8d91603fe35ffde4bfe0f401dda6a6739fd58))

## [1.21.0](https://github.com/happy-dwarf-team/anvil/compare/v1.20.0...v1.21.0) (2026-11-12)

### Features

* Modders get an inspector window from the new standard plugin `anvil-inspector`: turn on **Inspector window** (off by default) to see the flags, variables and timed modifiers of the open province, its owner and the player's country. Its texts in other languages are drafts. ([#330](https://github.com/happy-dwarf-team/anvil/issues/330)) ([6ee5c01](https://github.com/happy-dwarf-team/anvil/commit/6ee5c01a4ffd8355607bdd394f1b2c5fcc1caf49))
* **api:** `anvil-game.world` also reads the events waiting for a country's choice and the statistics of the game's graphs, in Rust through `anvil-sdk` 0.1.9. ([#329](https://github.com/happy-dwarf-team/anvil/issues/329)) ([df11a93](https://github.com/happy-dwarf-team/anvil/commit/df11a9386f95018b6ddc34226767155438b1c095))

## [1.20.0](https://github.com/happy-dwarf-team/anvil/compare/v1.19.0...v1.20.0) (2026-11-12)

### Features

* Modders can see the game's script errors in a panel while the game runs, from the standard plugin `anvil-log-throttle`. Turn on its new `panel` setting (off by default) in the overlay or Anvil Manager: the window then opens at each start, and once closed it comes back from the Status tab. It groups the errors by kind with their counts. First come the few that name a file and line, which *Open in VS Code* and *Copy* take you to. Last comes the start-up noise of missing files, folded into one item per kind; in Anbennar that was 18,740 of 18,831 messages. An undefined event target is one row, and *Find* looks for event targets and unknown tags in the playset's files. Plugins can read the same table through the new service `anvil-log-throttle.messages` (`anvil::log_throttle::Messages` in `anvil-sdk` 0.1.8). Tested in the lab on Steam 1.37.5: the panel's counts matched `error.log` for all 159 messages of a base-game campaign and all 18,760 of Anbennar's start-up, with the panel in Polish and Simplified Chinese. With the panel off, no cost against 1.19.0 was found, though two starts each cannot show one below a few per cent. Not tried in the game: *Open in VS Code*, *Copy*, *Find*, *Show only new* and *Mark all seen*, turning the setting on while the game runs, a save load with the window open, and OpenGL. The panel's texts in languages other than English are drafts that native speakers have not checked yet ([#318](https://github.com/happy-dwarf-team/anvil/issues/318)) ([5890af6](https://github.com/happy-dwarf-team/anvil/commit/5890af68ae66c3e6ebc46924ab52f88ae412c080))
* **api:** A mod can ask whether a plugin is running with the game's own trigger: `has_dlc = "anvil:<plugin id>"` (quoted, with the id as the plugin's `plugin.toml` writes it) is true when that plugin is loaded and not suspended in the current game. Without Anvil, outside a game, or with any other spelling, the game answers false and logs nothing, so a mod can gate on it and work unchanged for players without the plugin. No file the game loads changes, so the checksum stays the same. In multiplayer it is false on every machine, so all of them take the same branch. Tested in the lab on Steam 1.37.5 and GOG 1.37.5.2: an installed plugin answered true, a missing one false and a wrongly capitalised id false, real DLCs were still answered by the game, and without Anvil the mod loaded with no line in `error.log`. Not tested in the game: multiplayer (false there by design), ironman and a suspended plugin ([#321](https://github.com/happy-dwarf-team/anvil/issues/321)) ([e035589](https://github.com/happy-dwarf-team/anvil/commit/e03558908277bb1b543eb673a90e0b53bb611f27))
* **api:** Plugins can read rulers, heirs and consorts, flags and variables from `anvil-game.world`. `monarch` gives a ruler's, heir's or consort's id, skills, sex, regency, birth date, an heir's claim and the id of the leader the person also is, and `monarch_name` and `monarch_dynasty` give the names. A country without one answers `ANVIL_ERROR_NOT_FOUND`, never the game's shared stand-in. Flags of countries, provinces, rulers and the whole world come as lists with the date each was set, and country and province variables by name (×1000). Everything only reads, and Rust plugins get the same in `anvil-sdk` 0.1.8. Tested in the lab on Steam 1.37.5 and GOG 1.37.5.2 with the Russia save of 1719: the rulers, heirs and consorts of 15 countries, their flags and variables matched the save, and flags and variables set by an event showed up with their date and value. Flag ids differ between the builds, so a plugin looks them up by name in each game. On GOG 1.37.5.0 only the game addresses were checked; a regency council, an interregnum and a second campaign in the same process were not tried in the game ([#320](https://github.com/happy-dwarf-team/anvil/issues/320)) ([076f28b](https://github.com/happy-dwarf-team/anvil/commit/076f28b51cf1299246415d4dba91fce8e45b19a3))
* The overlay and Anvil Manager show plugins' settings and descriptions in the player's language. A plugin can ship `translations/<code>.toml` for French, German, Spanish, Simplified and Traditional Chinese, Japanese, Korean and Polish. An older Anvil ignores the folder, so a plugin needs no newer Anvil for it. The standard plugins have all eight, and so do the catalog's summaries of Bellows, Tongs, Hamweard and Mei; Chinese, Japanese and Korean are drawn with Windows' fonts. `anvil check` reports a broken or stale file, and with `--complete-translations` a missing one. Tested in the lab: in the game on Steam 1.37.5, the settings showed in Polish and Simplified Chinese; in the Manager, the settings in Simplified Chinese and the catalog in Korean. Not checked in the game: the other six languages, and a mod plugin's description in the approval dialog. All these translations are an agent's drafts that native speakers have not checked yet ([#322](https://github.com/happy-dwarf-team/anvil/issues/322)) ([00053d9](https://github.com/happy-dwarf-team/anvil/commit/00053d924a0878d6619765489f105da839c8746d))

### Bug Fixes

* **sdk:** The SDK zip now has `anvil_log_throttle.h`, the header of the new service `anvil-log-throttle.messages`, which the SDK's install list had left out, so a C or C++ plugin can include it. ([#324](https://github.com/happy-dwarf-team/anvil/issues/324)) ([a617cb7](https://github.com/happy-dwarf-team/anvil/commit/a617cb7f4a6e616212f15f260b74fceb894a66c3))

## [1.19.0](https://github.com/happy-dwarf-team/anvil/compare/v1.18.0...v1.19.0) (2026-11-10)

### Features

* **api:** Rust plugins can add script effects. `anvil-sdk` 0.1.7 wraps `anvil-script.effects` as `anvil::script::Effects`: `register` and `register_with_tooltip` take a handler that gets the effect's value and the province and country it runs in, and a tooltip handler whose first line goes into the game's tooltip. A panic in a handler is logged once and the game goes on. Tested with the crate's unit tests against a stand-in service; not run in the game, since the handlers reach the same C service as the C++ example `hello-script`, which was tested in the game for Anvil 1.16. ([#310](https://github.com/happy-dwarf-team/anvil/issues/310))

### Bug Fixes

* **api:** a script effect's tooltip line now shows exactly what the plugin wrote, as a map mode's lines do. The game's colour sign `§`, which the game cannot escape, shows as `?` instead of being dropped with the letter after it kept (`§Yred` showed as `Yred`), `$` is no longer dropped, and text that is not valid UTF-8 is cut at the first bad byte. Line breaks still end the line. Tested with Anvil's unit tests; not seen in the game yet: whether a decision's or an event's tooltip replaces a `$…$` in a plugin's line (a map mode's tooltip shows it as written). ([#311](https://github.com/happy-dwarf-team/anvil/issues/311))
* **api:** in `anvil-sdk`, a key binding whose key another plugin already holds is no longer lost. The new `Input::bind_or_inert` keeps it, inert, so the plugin can give it a free key later with `set_code`, for example when the player picks another key in the panel; `Input::bind` still answers `Conflict` as before. Each lost binding used to take one of the 256 a game may have. Tested with the crate's unit tests. ([#308](https://github.com/happy-dwarf-team/anvil/issues/308))

Nothing changes for players without plugins that use these services. The C and C++ interface is unchanged.

## [1.18.0](https://github.com/happy-dwarf-team/anvil/compare/v1.17.0...v1.18.0) (2026-11-10)


### Features

* **api:** add the anvil-overlay.input service for plugin keys ([#249](https://github.com/happy-dwarf-team/anvil/issues/249)) ([1f1c43c](https://github.com/happy-dwarf-team/anvil/commit/1f1c43c02f3b8606cabf187690858911afffffca))
* **api:** call gameplay plugins back at the day end through anvil-script ([#296](https://github.com/happy-dwarf-team/anvil/issues/296)) ([83eb418](https://github.com/happy-dwarf-team/anvil/commit/83eb41853c67e5535cd1adb5a6c7293b94d2f9d1))

## [1.17.0](https://github.com/happy-dwarf-team/anvil/compare/v1.16.0...v1.17.0) (2026-11-09)


### Features

* **api:** add anvil-game.commands, the unit panel's commands for gameplay plugins ([#297](https://github.com/happy-dwarf-team/anvil/issues/297)) ([014231a](https://github.com/happy-dwarf-team/anvil/commit/014231a7ff3496f2971d2133324b19d79e294e38))
* **api:** choose an event option through anvil-game.commands (version 2) ([#298](https://github.com/happy-dwarf-team/anvil/issues/298)) ([478df38](https://github.com/happy-dwarf-team/anvil/commit/478df3860b7037cb96a2b950fe3fd11d87890e3e))


### Bug Fixes

* **overlay:** subclass the game window before the device is made ([#292](https://github.com/happy-dwarf-team/anvil/issues/292)) ([dcad034](https://github.com/happy-dwarf-team/anvil/commit/dcad0349ef78c2cb0a8d80c749e29adc12b5fac7))

## [1.16.0](https://github.com/happy-dwarf-team/anvil/compare/v1.15.0...v1.16.0) (2026-11-09)


### Features

* **api:** add anvil-map, the standard plugin of plugin map modes ([#290](https://github.com/happy-dwarf-team/anvil/issues/290)) ([d9d8ff1](https://github.com/happy-dwarf-team/anvil/commit/d9d8ff1830f62e586147f7bc2f86a2bab508af44))
* **api:** safe Rust crate anvil-sdk with typed service wrappers ([#281](https://github.com/happy-dwarf-team/anvil/issues/281)) ([acb13e1](https://github.com/happy-dwarf-team/anvil/commit/acb13e1ced3200a56110d150442d8f3d48be389b))
* let gameplay plugins add script effects through anvil-script ([#282](https://github.com/happy-dwarf-team/anvil/issues/282)) ([4335f41](https://github.com/happy-dwarf-team/anvil/commit/4335f41afa5fc4bcc6836e15787ecf3b00db64b2))
* **log:** name the Wine and the d3d9.dll the game runs on in anvil.log ([#279](https://github.com/happy-dwarf-team/anvil/issues/279)) ([bbe0d88](https://github.com/happy-dwarf-team/anvil/commit/bbe0d88619d0d858fa8adca9d00d3bb8ad064be0))
* **manager:** replace EU4DLL and EU4 Unicode Patch with a backup ([#280](https://github.com/happy-dwarf-team/anvil/issues/280)) ([d2df34b](https://github.com/happy-dwarf-team/anvil/commit/d2df34b9782bac769f4b7a9538e6f25e3c1e6f88))
* **manager:** speak Chinese, Japanese or Korean from the first start ([#283](https://github.com/happy-dwarf-team/anvil/issues/283)) ([fa4dd22](https://github.com/happy-dwarf-team/anvil/commit/fa4dd2276604b45bd9b7d588ca7acffa86df9142))
* speak Polish in the Manager and the overlay ([#275](https://github.com/happy-dwarf-team/anvil/issues/275)) ([1bb0ea6](https://github.com/happy-dwarf-team/anvil/commit/1bb0ea67f4265020d4810c8877c02df48b85be0b))


### Bug Fixes

* **overlay:** say the crash in a whole sentence for each level ([#278](https://github.com/happy-dwarf-team/anvil/issues/278)) ([d4ea6c9](https://github.com/happy-dwarf-team/anvil/commit/d4ea6c9fdb1b4cbc278a169371a9265c15f93895))

## [1.15.0](https://github.com/happy-dwarf-team/anvil/compare/v1.14.1...v1.15.0) (2026-11-09)


### Features

* **api:** read modifiers from anvil-game.world ([#246](https://github.com/happy-dwarf-team/anvil/issues/246)) ([6b816e0](https://github.com/happy-dwarf-team/anvil/commit/6b816e0f59c130f1344a7b373df97694adf853df))
* **api:** read units and the selection from anvil-game.world ([#252](https://github.com/happy-dwarf-team/anvil/issues/252)) ([d26c34e](https://github.com/happy-dwarf-team/anvil/commit/d26c34eeaab95aacd304bfce2b0532c32a17639b))
* know the GOG build 1.37.5.0 ([#273](https://github.com/happy-dwarf-team/anvil/issues/273)) ([880da87](https://github.com/happy-dwarf-team/anvil/commit/880da87d4fa472871d6fea5cd34d86509d28a5ce)), closes [#187](https://github.com/happy-dwarf-team/anvil/issues/187)
* **manager:** read the plugin catalog at start and show plugin updates ([#270](https://github.com/happy-dwarf-team/anvil/issues/270)) ([9676b2b](https://github.com/happy-dwarf-team/anvil/commit/9676b2b67fee26c5647ba6c0d55f0556079bec1a))

## [1.14.1](https://github.com/happy-dwarf-team/anvil/compare/v1.14.0...v1.14.1) (2026-11-09)


### Bug Fixes

* **overlay:** show each settings group once, in the overlay and the Manager ([#266](https://github.com/happy-dwarf-team/anvil/issues/266)) ([9c7e115](https://github.com/happy-dwarf-team/anvil/commit/9c7e115208e28e0a06c758b2ddcdcb5514fb6f24))

## [1.14.0](https://github.com/happy-dwarf-team/anvil/compare/v1.13.0...v1.14.0) (2026-11-09)


### Features

* **api:** report out-of-sync state from anvil-game ([#244](https://github.com/happy-dwarf-team/anvil/issues/244)) ([8dd2e76](https://github.com/happy-dwarf-team/anvil/commit/8dd2e76c9b790429af3da0461d570812679355f2))
* **manager:** say when the game ran but Anvil did not load ([#254](https://github.com/happy-dwarf-team/anvil/issues/254)) ([8e3a386](https://github.com/happy-dwarf-team/anvil/commit/8e3a3864d30da67d2048145cc9f15b6fc8542967)), closes [#253](https://github.com/happy-dwarf-team/anvil/issues/253)
* **overlay:** tell the player about a newer Anvil release ([#260](https://github.com/happy-dwarf-team/anvil/issues/260)) ([b5ab3bc](https://github.com/happy-dwarf-team/anvil/commit/b5ab3bc878ef34428c7793354ff0fea55aa3b36c))


### Bug Fixes

* **overlay:** let the player close the multiplayer notice ([#257](https://github.com/happy-dwarf-team/anvil/issues/257)) ([f8f4338](https://github.com/happy-dwarf-team/anvil/commit/f8f4338927901f65e338bb2073cf3ab2605d7cda))

## [1.13.0](https://github.com/happy-dwarf-team/anvil/compare/v1.12.0...v1.13.0) (2026-11-06)


### Features

* **manager:** add a Start menu shortcut and bring back the dark background ([#239](https://github.com/happy-dwarf-team/anvil/issues/239)) ([b91ec3d](https://github.com/happy-dwarf-team/anvil/commit/b91ec3d918d0cd2b2744d71bca752d4779d899bb))

## [1.12.0](https://github.com/happy-dwarf-team/anvil/compare/v1.11.0...v1.12.0) (2026-11-06)


### Features

* **cli:** make anvil port take --build from the table of builds ([#232](https://github.com/happy-dwarf-team/anvil/issues/232)) ([f6e2cce](https://github.com/happy-dwarf-team/anvil/commit/f6e2cce527048b7262983def2a2de13802b6efc5))


### Bug Fixes

* **cli:** port a digest whose value moved instead of leaving it by hand ([#236](https://github.com/happy-dwarf-team/anvil/issues/236)) ([2911a02](https://github.com/happy-dwarf-team/anvil/commit/2911a02b7ded7461a8b0d0d1a41d10fb6d0b4b6c))
* **i18n:** say that a plugin does not support the game version yet ([#235](https://github.com/happy-dwarf-team/anvil/issues/235)) ([0750ac2](https://github.com/happy-dwarf-team/anvil/commit/0750ac2a1742191efa141136df3457b6a251d2a6))
* limit what is read of a plugin, and warn when a mod presents itself as a catalog plugin ([#233](https://github.com/happy-dwarf-team/anvil/issues/233)) ([ecea7e2](https://github.com/happy-dwarf-team/anvil/commit/ecea7e23aa93459f16f4eb93fb6e7797c637f737))
* **manager:** show an update's own files in the approve dialog ([#226](https://github.com/happy-dwarf-team/anvil/issues/226)) ([7c62d0d](https://github.com/happy-dwarf-team/anvil/commit/7c62d0d3a8de61dbfb8ecdecf02953a0a760ef56))
* **manager:** show an update's own problems on the Mods tab ([#229](https://github.com/happy-dwarf-team/anvil/issues/229)) ([f610229](https://github.com/happy-dwarf-team/anvil/commit/f610229043f6866191574000bf0754278f788892))
* **overlay:** say in one notice that stays which plugins wait for this game version ([#231](https://github.com/happy-dwarf-team/anvil/issues/231)) ([7435ca3](https://github.com/happy-dwarf-team/anvil/commit/7435ca3529b09254903db6a3c923f616a76822b4))


### Build

* **manager:** build anvil-log-throttle before the package ([#230](https://github.com/happy-dwarf-team/anvil/issues/230)) ([b47d17a](https://github.com/happy-dwarf-team/anvil/commit/b47d17ae50e750271ddb56e0a901fb261b02a320))

## [1.11.0](https://github.com/happy-dwarf-team/anvil/compare/v1.10.1...v1.11.0) (2026-11-05)


### Features

* **api:** add plugin windows to the overlay ([#225](https://github.com/happy-dwarf-team/anvil/issues/225)) ([b8136b2](https://github.com/happy-dwarf-team/anvil/commit/b8136b2d8a3fc57ba9b6cc1001dab233c6c76f07))
* **api:** read the world and hear the date in anvil-game ([#219](https://github.com/happy-dwarf-team/anvil/issues/219)) ([a6d5f12](https://github.com/happy-dwarf-team/anvil/commit/a6d5f12f96468f00bd22e4c631c055e90c799e59))
* **api:** report wars in anvil-game ([#223](https://github.com/happy-dwarf-team/anvil/issues/223)) ([e76bb87](https://github.com/happy-dwarf-team/anvil/commit/e76bb878650f329ba7cf3e8088e15ef28be4f176))
* **cli:** add anvil port to place symbols in a new game build ([#218](https://github.com/happy-dwarf-team/anvil/issues/218)) ([02c43e9](https://github.com/happy-dwarf-team/anvil/commit/02c43e93dba0fbf2f5898ab15908d57efcc2bcdd))


### Bug Fixes

* **overlay:** say once that the game build is unknown and keep it until closed ([#220](https://github.com/happy-dwarf-team/anvil/issues/220)) ([c8b1bd6](https://github.com/happy-dwarf-team/anvil/commit/c8b1bd6651fd10e7f41481881fc6937ee2885adf))
* read a -userdir whose path alone is in quotes ([#222](https://github.com/happy-dwarf-team/anvil/issues/222)) ([0091a96](https://github.com/happy-dwarf-team/anvil/commit/0091a963d7747456c9feff1cfb057208227ddfee))


### Build

* **overlay:** update Dear ImGui to 1.93.0 ([#224](https://github.com/happy-dwarf-team/anvil/issues/224)) ([a419ef7](https://github.com/happy-dwarf-team/anvil/commit/a419ef78137367d345f17e31ef92150fae5d9f2a))

## [1.10.1](https://github.com/happy-dwarf-team/anvil/compare/v1.10.0...v1.10.1) (2026-11-04)


### Bug Fixes

* **cli:** do not warn about plugins that declare the same address ([#209](https://github.com/happy-dwarf-team/anvil/issues/209)) ([10ff00e](https://github.com/happy-dwarf-team/anvil/commit/10ff00e4fe94f4179955613c56761691eb36c4a8))
* **manager:** say what Play does on GOG ([#211](https://github.com/happy-dwarf-team/anvil/issues/211)) ([c89792c](https://github.com/happy-dwarf-team/anvil/commit/c89792c8813b4e813e10d55fdd12531d1857ce55))
* **manager:** show a scrollbar when a tab does not fit ([#213](https://github.com/happy-dwarf-team/anvil/issues/213)) ([37e4e50](https://github.com/happy-dwarf-team/anvil/commit/37e4e500de2e6e3fce9e6a082503f3bd2a91cb5c))

## [1.10.0](https://github.com/happy-dwarf-team/anvil/compare/v1.9.0...v1.10.0) (2026-11-04)


### Features

* **cli:** verify symbols by SHA-256 instead of public bytes ([#203](https://github.com/happy-dwarf-team/anvil/issues/203)) ([3ca95f7](https://github.com/happy-dwarf-team/anvil/commit/3ca95f7ec0ffb27f9e232c2bc14ab11677859395))
* **manager:** find and start a GOG install ([#199](https://github.com/happy-dwarf-team/anvil/issues/199)) ([c66abba](https://github.com/happy-dwarf-team/anvil/commit/c66abba7e96e089d298c0e2eded2266aaebe9668))
* run anvil-log-throttle on the GOG build 1.37.5.2 ([#205](https://github.com/happy-dwarf-team/anvil/issues/205)) ([ff34e66](https://github.com/happy-dwarf-team/anvil/commit/ff34e66c44993e37f14c00942bd79d1c3e9b1f84))
* tell a GOG player how to get a build Anvil knows ([#200](https://github.com/happy-dwarf-team/anvil/issues/200)) ([72a3c39](https://github.com/happy-dwarf-team/anvil/commit/72a3c39baaab4bc4ab5760fa5a9fd837075ec4fb))


### Bug Fixes

* **manager:** clearer messages without a game or a known build ([#206](https://github.com/happy-dwarf-team/anvil/issues/206)) ([56ba08b](https://github.com/happy-dwarf-team/anvil/commit/56ba08b1ade99ab6d302acd7ca339b8f76456edf))
* **manager:** offer the administrator when a plugin cannot be installed for lack of rights ([#198](https://github.com/happy-dwarf-team/anvil/issues/198)) ([16e2b04](https://github.com/happy-dwarf-team/anvil/commit/16e2b04cca83b0341453b066f7424b9b805fc990))
* read a GOG install's own user folder ([#197](https://github.com/happy-dwarf-team/anvil/issues/197)) ([0040050](https://github.com/happy-dwarf-team/anvil/commit/0040050830b1bed3502e11556ac74a0e04042c55))
* read the mods and the language from the folder -userdir names ([#207](https://github.com/happy-dwarf-team/anvil/issues/207)) ([a5ddc6f](https://github.com/happy-dwarf-team/anvil/commit/a5ddc6f97a218ebb95a30218512e9445e36a01e4))

## [1.9.0](https://github.com/happy-dwarf-team/anvil/compare/v1.8.0...v1.9.0) (2026-11-03)


### Features

* let a symbol be absent on one of several builds ([#194](https://github.com/happy-dwarf-team/anvil/issues/194)) ([bbe3f0d](https://github.com/happy-dwarf-team/anvil/commit/bbe3f0de47c747023a36acfa14c21eec6a8b5924))
* **manager:** copy a problem report for an issue from the Log tab ([#190](https://github.com/happy-dwarf-team/anvil/issues/190)) ([88fba77](https://github.com/happy-dwarf-team/anvil/commit/88fba7719ba812effd34f576e8f5aa9b113ee07a))
* **manager:** install the team's plugins from a catalog tab ([#189](https://github.com/happy-dwarf-team/anvil/issues/189)) ([bd8f12e](https://github.com/happy-dwarf-team/anvil/commit/bd8f12e3a07685a174fde72f2ddab71ff2d6d91b))
* show the overlay and the Manager in Chinese, Japanese and Korean ([#192](https://github.com/happy-dwarf-team/anvil/issues/192)) ([71baccb](https://github.com/happy-dwarf-team/anvil/commit/71baccbcfd5398af41d0deefe1d35c876421d828))

## [1.8.0](https://github.com/happy-dwarf-team/anvil/compare/v1.7.0...v1.8.0) (2026-11-02)


### Features

* show Anvil's version in the game's main menu and open the panel with a click ([#184](https://github.com/happy-dwarf-team/anvil/issues/184)) ([4d757a2](https://github.com/happy-dwarf-team/anvil/commit/4d757a2ae2734edad417573be86836d74bb8a43b))

## [1.7.0](https://github.com/happy-dwarf-team/anvil/compare/v1.6.1...v1.7.0) (2026-11-02)


### Features

* show player messages in the player's language ([#181](https://github.com/happy-dwarf-team/anvil/issues/181)) ([16d37cd](https://github.com/happy-dwarf-team/anvil/commit/16d37cdbb0bd7c97fd1d60fbfaf7fe1a475d5779))
* support the GOG build 1.37.5.2 and symbols for several builds ([#182](https://github.com/happy-dwarf-team/anvil/issues/182)) ([37350be](https://github.com/happy-dwarf-team/anvil/commit/37350be27a945410b2a27a130168b21af0c77d28))

## [1.6.1](https://github.com/happy-dwarf-team/anvil/compare/v1.6.0...v1.6.1) (2026-11-02)


### Bug Fixes

* **log-throttle:** name a message in anvil.log without its line break ([#176](https://github.com/happy-dwarf-team/anvil/issues/176)) ([f8673e6](https://github.com/happy-dwarf-team/anvil/commit/f8673e6b79919750082726040d5d78fc1fdaa701))

## [1.6.0](https://github.com/happy-dwarf-team/anvil/compare/v1.5.2...v1.6.0) (2026-11-01)


### Features

* **api:** offer the current game date in anvil-game.game ([#171](https://github.com/happy-dwarf-team/anvil/issues/171)) ([fdf3f4e](https://github.com/happy-dwarf-team/anvil/commit/fdf3f4e32c035c4c21bbc8f5c6b002c134784892))
* **log-throttle:** throttle repeated lines of the game's logs ([#173](https://github.com/happy-dwarf-team/anvil/issues/173)) ([6110566](https://github.com/happy-dwarf-team/anvil/commit/6110566324ed4faf4f1b5e8ca666e9ff1bb6a71d))

## [1.5.2](https://github.com/happy-dwarf-team/anvil/compare/v1.5.1...v1.5.2) (2026-10-26)


### Bug Fixes

* **core:** write the exit work from the game's ExitProcess ([#165](https://github.com/happy-dwarf-team/anvil/issues/165)) ([4155d4f](https://github.com/happy-dwarf-team/anvil/commit/4155d4f7614872b13a3c4af0d34ed61dc754d814)), closes [#164](https://github.com/happy-dwarf-team/anvil/issues/164)
* **game:** publish the session events of a save loaded from the game menu ([#169](https://github.com/happy-dwarf-team/anvil/issues/169)) ([13e5a4b](https://github.com/happy-dwarf-team/anvil/commit/13e5a4b7b0f1ab0e2517a8e7d5a7643f516eb365))

## [1.5.1](https://github.com/happy-dwarf-team/anvil/compare/v1.5.0...v1.5.1) (2026-10-26)


### Bug Fixes

* **manager:** open the privacy note on anvil.happydwarf.dev ([#156](https://github.com/happy-dwarf-team/anvil/issues/156)) ([a93a07e](https://github.com/happy-dwarf-team/anvil/commit/a93a07e97a00b651dec0f68200e707b8f11af951))

## [1.5.0](https://github.com/happy-dwarf-team/anvil/compare/v1.4.0...v1.5.0) (2026-10-26)


### Features

* **manager:** add the Benchmark tab ([#151](https://github.com/happy-dwarf-team/anvil/issues/151)) ([b0f2dae](https://github.com/happy-dwarf-team/anvil/commit/b0f2daef32f401f5f7792bb2276618c5220ceaa7))
* **manager:** run a benchmark series of game starts ([#149](https://github.com/happy-dwarf-team/anvil/issues/149)) ([aa867f3](https://github.com/happy-dwarf-team/anvil/commit/aa867f34d7a7d8c4b2dc7bf9efd66c9e5a28cedb))
* **manager:** send a benchmark report to the receiver ([#152](https://github.com/happy-dwarf-team/anvil/issues/152)) ([bc8ce69](https://github.com/happy-dwarf-team/anvil/commit/bc8ce693c5253afef5b567ce336180bd5f2b44d8))
* **manager:** write and keep the benchmark report format 1 ([#148](https://github.com/happy-dwarf-team/anvil/issues/148)) ([168744b](https://github.com/happy-dwarf-team/anvil/commit/168744b7c8f8c2f862e28336190bd03c338c7a6d))

## [1.4.0](https://github.com/happy-dwarf-team/anvil/compare/v1.3.0...v1.4.0) (2026-10-25)


### Features

* **api:** add the anvil.benchmark v1 service for plugin fields ([#138](https://github.com/happy-dwarf-team/anvil/issues/138)) ([12386e7](https://github.com/happy-dwarf-team/anvil/commit/12386e7861b4ed64eba7784191a163cc344336b7))
* **core:** read the benchmark arm and leave the compared plugin out ([#141](https://github.com/happy-dwarf-team/anvil/issues/141)) ([f751cff](https://github.com/happy-dwarf-team/anvil/commit/f751cff8b9274d69b26e2dd2b8f73a7b70192842))
* **core:** write the benchmark start record with open cost and labels ([#142](https://github.com/happy-dwarf-team/anvil/issues/142)) ([f26c978](https://github.com/happy-dwarf-team/anvil/commit/f26c97803ac7f062af4e38fdeb88008ae8bcefc0))
* **game:** quit a benchmark start once every participant is done ([#143](https://github.com/happy-dwarf-team/anvil/issues/143)) ([e5559b3](https://github.com/happy-dwarf-team/anvil/commit/e5559b3c8a7ccb8b29fe27fd2da8f0a0cbbf9325))
* **manager:** classify benchmark starts and compute the series result ([#147](https://github.com/happy-dwarf-team/anvil/issues/147)) ([9e97c38](https://github.com/happy-dwarf-team/anvil/commit/9e97c38c40b8f1f5c4a11a14d68317d9be12600f))

## [1.3.0](https://github.com/happy-dwarf-team/anvil/compare/v1.2.1...v1.3.0) (2026-10-20)


### Features

* **core:** discover plugins in enabled mods and load approved ones ([#113](https://github.com/happy-dwarf-team/anvil/issues/113)) ([74fce2d](https://github.com/happy-dwarf-team/anvil/commit/74fce2d96c1a3a7eb0584fd0c01786e88ca4fb49))
* **manager:** approve, revoke and show plugins from mods ([#116](https://github.com/happy-dwarf-team/anvil/issues/116)) ([2484cc2](https://github.com/happy-dwarf-team/anvil/commit/2484cc219a5091eb081ca3ab2d6d26df5ce2daab))
* **overlay:** approve and revoke plugins from mods in the panel ([#114](https://github.com/happy-dwarf-team/anvil/issues/114)) ([e825771](https://github.com/happy-dwarf-team/anvil/commit/e8257713824c1b5f9bedf61ccbbcad47bb6a422d))


### Bug Fixes

* **crash:** report a stack overflow on the game's other threads where Windows enables AMX ([#119](https://github.com/happy-dwarf-team/anvil/issues/119)) ([baf9bc8](https://github.com/happy-dwarf-team/anvil/commit/baf9bc8784c438be53249236aba08ea058fb64cd))

## [1.2.1](https://github.com/happy-dwarf-team/anvil/compare/v1.2.0...v1.2.1) (2026-10-20)


### Bug Fixes

* **manager:** accept posted mouse messages from automated tests ([#111](https://github.com/happy-dwarf-team/anvil/issues/111)) ([11243d0](https://github.com/happy-dwarf-team/anvil/commit/11243d0bd5d69988668074af00d648d3aa087bfa))
* **manager:** remove an empty anvil/crashes folder on uninstall ([#110](https://github.com/happy-dwarf-team/anvil/issues/110)) ([245935e](https://github.com/happy-dwarf-team/anvil/commit/245935e273c7c0f9358077309314f4fe06282d31))
* release the standard plugins at Anvil's version ([#109](https://github.com/happy-dwarf-team/anvil/issues/109)) ([be510e8](https://github.com/happy-dwarf-team/anvil/commit/be510e89b5c2defb550611080f8082987761ce23))

## [1.2.0](https://github.com/happy-dwarf-team/anvil/compare/v1.1.0...v1.2.0) (2026-10-20)


### Features

* **manager:** add AnvilManager.exe, the player's installer and management tool ([#101](https://github.com/happy-dwarf-team/anvil/issues/101)) ([5f57b1a](https://github.com/happy-dwarf-team/anvil/commit/5f57b1a0079326df8ad3e436f5ad6ce77275cf57))
* **manager:** add the Manager's core with game detection, install plans and plugin zips ([#100](https://github.com/happy-dwarf-team/anvil/issues/100)) ([a0981fa](https://github.com/happy-dwarf-team/anvil/commit/a0981faa4fb0ed402c95702d441582664c3297a4))
* **release:** ship AnvilManager.exe as an asset and in the Anvil zip ([#102](https://github.com/happy-dwarf-team/anvil/issues/102)) ([ef139a9](https://github.com/happy-dwarf-team/anvil/commit/ef139a9d0d77c154d1c0fee6373bd7e826dfe493))

## [1.1.0](https://github.com/happy-dwarf-team/anvil/compare/v1.0.0...v1.1.0) (2026-10-20)


### Features

* **core:** add the private anvil.control service for the overlay ([#93](https://github.com/happy-dwarf-team/anvil/issues/93)) ([46f611e](https://github.com/happy-dwarf-team/anvil/commit/46f611e40fab81f3b8edc5fe37d6ec76dc5fb199))
* **overlay:** add the anvil-overlay standard plugin ([#96](https://github.com/happy-dwarf-team/anvil/issues/96)) ([bb19b7a](https://github.com/happy-dwarf-team/anvil/commit/bb19b7a0dfeab2d75e9596f36e7dfa4c4eae9984))


### Bug Fixes

* **core:** report a crash in a mid hook's or block replacement's callback ([#95](https://github.com/happy-dwarf-team/anvil/issues/95)) ([3d292de](https://github.com/happy-dwarf-team/anvil/commit/3d292deca5e72b469ab0355e3b18d4deda9eceb1))


### Performance

* **core:** share one context stub so mid hooks and block replacements fit 1 KB ([#97](https://github.com/happy-dwarf-team/anvil/issues/97)) ([3ccf840](https://github.com/happy-dwarf-team/anvil/commit/3ccf840845168b01c6451ada3ffcafc0ce6c820c)), closes [#90](https://github.com/happy-dwarf-team/anvil/issues/90)
* **core:** trim the heap of every change, most of all mid hooks and block replacements ([#92](https://github.com/happy-dwarf-team/anvil/issues/92)) ([c4582f3](https://github.com/happy-dwarf-team/anvil/commit/c4582f33a881bdaf0d430774db3e903ff9d5830d)), closes [#90](https://github.com/happy-dwarf-team/anvil/issues/90)

## [1.0.0](https://github.com/happy-dwarf-team/anvil/compare/v0.2.0...v1.0.0) (2026-10-20)


### ⚠ BREAKING CHANGES

* **api:** plugin API 1 is removed. Plugins include the new anvil_plugin.h (or anvil.hpp), declare every eu4.exe address in symbols.toml and pass symbol ids (SYM_* / sym::*) instead of RVAs with expected bytes; hook_function, redirect_call and replace_import return their own `next`; expect_bytes, alloc_near_image, ANVIL_EVENT_GAME_READY and the PE fields of AnvilGameInfo are gone; result codes are renumbered.
* **core:** changes an API 1 plugin requests in anvil_plugin_load are enabled only after it returns ANVIL_OK. A second hook, redirect or import replacement at the same address joins a chain instead of failing with ANVIL_ERROR_CONFLICT, expected bytes are compared with the original bytes, and an overlap with the plugin's own change at another address is refused.
* **manifest:** plugin.toml must declare api = 2 and affects_gameplay, and must not use game_builds, [[expect]] or any other unknown key. Plugins with api = 1 are skipped.

### Features

* **anvil-game:** readiness, frame, main-thread work and the gameplay policy ([#77](https://github.com/happy-dwarf-team/anvil/issues/77)) ([376bc82](https://github.com/happy-dwarf-team/anvil/commit/376bc82a70f6c3e64117bd2f2151b5145a9bc7c0)), closes [#54](https://github.com/happy-dwarf-team/anvil/issues/54)
* **api:** plugin API 2 header, C++ wrapper and hello examples ([#74](https://github.com/happy-dwarf-team/anvil/issues/74)) ([5ba11a1](https://github.com/happy-dwarf-team/anvil/commit/5ba11a193802f973effd7100b6ed24b49971abeb)), closes [#46](https://github.com/happy-dwarf-team/anvil/issues/46)
* **cli:** anvil check --game ([#68](https://github.com/happy-dwarf-team/anvil/issues/68)) ([ab63642](https://github.com/happy-dwarf-team/anvil/commit/ab6364209627634d6b5669d6edb5946395db83fa))
* **cli:** anvil gen and anvil check ([#67](https://github.com/happy-dwarf-team/anvil/issues/67)) ([63da5a4](https://github.com/happy-dwarf-team/anvil/commit/63da5a4c18c8bb364c184abae15fe5e2535cdc9e)), closes [#41](https://github.com/happy-dwarf-team/anvil/issues/41)
* **core:** change registry with Anvil-owned chains, staging and handles ([#66](https://github.com/happy-dwarf-team/anvil/issues/66)) ([245591c](https://github.com/happy-dwarf-team/anvil/commit/245591c8539c0741ea3c601a5384fbad182d1c09))
* **core:** crash capture and attribution with safe mode ([#79](https://github.com/happy-dwarf-team/anvil/issues/79)) ([7fae2b5](https://github.com/happy-dwarf-team/anvil/commit/7fae2b57a7b092b6106f55d88437eadcb8e91a49))
* **core:** mid hooks with AnvilCpuContext and block replacement ([#69](https://github.com/happy-dwarf-team/anvil/issues/69)) ([6988aef](https://github.com/happy-dwarf-team/anvil/commit/6988aef9eaef1594517a16d1f4893443b5b1e04b)), closes [#43](https://github.com/happy-dwarf-team/anvil/issues/43)
* **core:** optional per-change call counters ([#73](https://github.com/happy-dwarf-team/anvil/issues/73)) ([1440cd5](https://github.com/happy-dwarf-team/anvil/commit/1440cd57d4ae664fc8ca25f4052d450fed3964ac))
* **core:** plugin settings ([#78](https://github.com/happy-dwarf-team/anvil/issues/78)) ([893b3ce](https://github.com/happy-dwarf-team/anvil/commit/893b3ce258e270d6eb5760c1a2e7138f5cdba8cb)), closes [#48](https://github.com/happy-dwarf-team/anvil/issues/48)
* **core:** pointer swap ([#71](https://github.com/happy-dwarf-team/anvil/issues/71)) ([c3d7c0f](https://github.com/happy-dwarf-team/anvil/commit/c3d7c0febe229873a75b2791555a2b30130e942d))
* **core:** skip plugins that declare a conflict with an enabled plugin ([#65](https://github.com/happy-dwarf-team/anvil/issues/65)) ([f657df5](https://github.com/happy-dwarf-team/anvil/commit/f657df5175c823c92a7423656c35264129989f66))
* **core:** suspend and resume all changes of a plugin ([#72](https://github.com/happy-dwarf-team/anvil/issues/72)) ([9dc06ce](https://github.com/happy-dwarf-team/anvil/commit/9dc06ce117391054fc099220d652300ea7c7eeb2))
* **manifest:** manifest v2 ([#62](https://github.com/happy-dwarf-team/anvil/issues/62)) ([800232b](https://github.com/happy-dwarf-team/anvil/commit/800232bbc4a45f6a42f25ef565c6538b54a5df97))
* **rust:** anvil-sys, anvil gen --lang rust and a Rust hello ([#76](https://github.com/happy-dwarf-team/anvil/issues/76)) ([ab8f7e6](https://github.com/happy-dwarf-team/anvil/commit/ab8f7e699b408e94f117f2b8d5f89e19773d0114))
* **symbols:** parse and verify symbols.toml ([#64](https://github.com/happy-dwarf-team/anvil/issues/64)) ([8b8b50f](https://github.com/happy-dwarf-team/anvil/commit/8b8b50fce06f6abce5fae940d331b487f583368e))


### Bug Fixes

* **build:** let the SDK from source work without the C language ([#85](https://github.com/happy-dwarf-team/anvil/issues/85)) ([51c8ac4](https://github.com/happy-dwarf-team/anvil/commit/51c8ac46350f2598c800c7f52bf85d21ec163b39)), closes [#81](https://github.com/happy-dwarf-team/anvil/issues/81)
* **cli:** check value names the generators reject ([#84](https://github.com/happy-dwarf-team/anvil/issues/84)) ([f56b442](https://github.com/happy-dwarf-team/anvil/commit/f56b442287c6901b493ad3bb7975ac40e9bbd397)), closes [#80](https://github.com/happy-dwarf-team/anvil/issues/80)
* **core:** keep the safe-mode count and finish the minidump after a crash ([#83](https://github.com/happy-dwarf-team/anvil/issues/83)) ([13db38d](https://github.com/happy-dwarf-team/anvil/commit/13db38d7aeefd731c6b468844a16f30b67b1de61)), closes [#82](https://github.com/happy-dwarf-team/anvil/issues/82)
* **examples:** add the symbols.lock of hello-rust ([#88](https://github.com/happy-dwarf-team/anvil/issues/88)) ([e38435e](https://github.com/happy-dwarf-team/anvil/commit/e38435eee4d4aae795c91ff8c4c951a0f96394d6)), closes [#86](https://github.com/happy-dwarf-team/anvil/issues/86)


### Build

* add the SDK archive and CMake package ([#75](https://github.com/happy-dwarf-team/anvil/issues/75)) ([cc84234](https://github.com/happy-dwarf-team/anvil/commit/cc84234980b6e02649b2a68c7befffb6de793063)), closes [#53](https://github.com/happy-dwarf-team/anvil/issues/53)


### Documentation

* give mid hooks and block replacements a 2 KB memory budget ([#89](https://github.com/happy-dwarf-team/anvil/issues/89)) ([f25b1d7](https://github.com/happy-dwarf-team/anvil/commit/f25b1d703262668beb76bfd4a1b3407ef7dd7f08)), closes [#57](https://github.com/happy-dwarf-team/anvil/issues/57)

## [0.2.0](https://github.com/happy-dwarf-team/anvil/compare/v0.1.0...v0.2.0) (2026-10-15)


### Features

* **api:** verify dependency bytes before patching ([#1](https://github.com/happy-dwarf-team/anvil/issues/1)) ([b78072d](https://github.com/happy-dwarf-team/anvil/commit/b78072df7c38374024526a2858fe8ca8ad899ea5))
* **manifest:** let plugins require a minimum Anvil version ([#3](https://github.com/happy-dwarf-team/anvil/issues/3)) ([668ff79](https://github.com/happy-dwarf-team/anvil/commit/668ff79d9c8a5617c7848b3bca8bdcd738346def))


### Build

* read the version from version.txt only ([#2](https://github.com/happy-dwarf-team/anvil/issues/2)) ([f24ae77](https://github.com/happy-dwarf-team/anvil/commit/f24ae770930af446c490f83b53b096d949982e07))
