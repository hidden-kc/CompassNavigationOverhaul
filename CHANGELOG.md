# CompassNavigationOverhaul - changelog

Rule 61: this mod's own history, kept beside the code it describes.

> **The entries below this line were RECONSTRUCTED from `version-ledger.json` on
> 2026-08-27, not written at the time of the change.** They carry only what the ledger
> recorded - the status and the evidence - so they are thinner than a real entry and may
> be missing changes the ledger never captured. Treat them as a starting point rather
> than a record. Everything from the next version onward is written as it happens.

Each version carries its **version-ledger status**: **working** (observed in game),
**untested** (built, not confirmed), **failed** (built but broken; the number was
reclaimed), **scratch** (a hypothesis-test build that never held a real number).

<!-- VERSIONING-RULES -->
> **Versioning rules (CLAUDE.md rules 6 and 48 - identical for mods and documents):**
> * `X.Y.Z`. A change increments the THIRD number. At `.9` the MINOR rolls: `1.0.9 -> 1.1.0`;
>   `1.0.10` never exists.
> * The next number is **LAST WORKING + 1**. A failed, scratch or untested test build does NOT
>   consume its number - the next attempt at the same step REUSES it.
> * Numbers are assigned by the tooling, never by hand: mods via `version-ledger.ps1 -Action next`
>   then `set-version.ps1`; governed documents via `docs-pipeline.ps1 -Action bump`; the rules via
>   `rules-version.ps1 -Action bump`. If a number was typed by hand, it is wrong until the tool
>   agrees.

## 1.1.1 - 2026-08-27 - untested

### Fixed
- Settings saved in game were lost on reload: Save() wrote the INI with plain file I/O, but Init() and Reload() read it back through INISettingCollection::ReadFromFile, which uses the Win32 profile API that PrivateProfileRedirector hooks and caches - so a reload was served the values from game start, and the Redirector could later flush its stale cache back over the file. Settings are now parsed straight from the INI with plain file I/O and preferred over the collection, which is left holding only the compiled-in defaults; the plugin never hands its INI to the profile API in either direction, so it behaves identically with or without the Redirector installed. Same fix as Dragon's Eye Minimap 1.5.7.

## 1.1.0 - 2026-08-27 - working

### Changed
- published on Nexus 189628 (file_id 795461, MAIN, 2026-08-26); git tag v1.1.0 - LOCKED. Note the tag sits on the commit whose CMakeLists still reads 1.0.10

### Known
- Ships a dedicated CoMAP compatibility patch - hooks::compat::MapMarkerFramework in source/MessageListeners.cpp and include/Hooks.h - gated on CoMAP's own SKSE plugin version. This is why the 2026-08-27 compatibility pass recorded CoMAP as compatible: the integration is deliberate, not incidental. Also carries the rule 6 retroactive renumbering (a v1.0.10 was published before the pre-push hook existed to reject a patch component of 10; sorting 1.0.10 against 1.0.9 as strings puts them in the wrong order).
- LATENT BUG, verified by source inspection 2026-08-27, not yet fixed here: this mod saves its INI with plain file I/O but reads it back through INISettingCollection::ReadFromFile, which uses the Win32 profile API that PrivateProfileRedirector hooks and caches. Under the Redirector a reload is served the values from game start rather than the ones just written - settings appear to save then revert. Worse, once the plugin's INI has been read through that API the Redirector caches it and can write its stale copy back over the file on game-save or exit, losing settings between sessions. Dragon's Eye Minimap 1.5.7 fixed exactly this: prefer values parsed directly from the file in the shared Read<T> helper, and stop calling ReadFromFile so the Redirector never caches our INI at all. CustomDifficultyUI-SMF was fixed earlier and is the precedent.

## 1.0.9 - 2026-08-27 - working

### Changed
- PROGRESS.md in-game test 2026-08-26 - swallow confirmed firing on all four directions, toggle colours confirmed by Liam

## 1.0.8 - 2026-08-27 - untested

### Changed
- local package only - no tag; superseded the same development cycle

## 1.0.7 - 2026-08-27 - untested

### Changed
- local package only - no tag; the arrow-key swallow, reverted before 1.1.0

## 1.0.6 - 2026-08-27 - working

### Changed
- published on Nexus 189628 (file_id 795107/795110, OLD_VERSION, 2026-08-25); git tag v1.0.6 - LOCKED, do not renumber

## 1.0.5 - 2026-08-27 - working

### Changed
- git tag v1.0.5 pushed

## 1.0.4 - 2026-08-27 - working

### Changed
- git tag v1.0.4 pushed

## 1.0.3 - 2026-08-27 - working

### Changed
- git tag v1.0.3 pushed; real cause fixed (CoMAP compat patch unchecked failure paths)

## 1.0.2 - 2026-08-27 - failed

### Known
- Ships a dedicated CoMAP compatibility patch - hooks::compat::MapMarkerFramework in source/MessageListeners.cpp and include/Hooks.h - gated on CoMAP's own SKSE plugin version. This is why the 2026-08-27 compatibility pass recorded CoMAP as compatible: the integration is deliberate, not incidental. Also carries the rule 6 retroactive renumbering (a v1.0.10 was published before the pre-push hook existed to reject a patch component of 10; sorting 1.0.10 against 1.0.9 as strings puts them in the wrong order).
- same trampoline crash recurred unchanged - the 1.0.2 fix was a misdiagnosis

## 1.0.1 - 2026-08-27 - failed

### Known
- Ships a dedicated CoMAP compatibility patch - hooks::compat::MapMarkerFramework in source/MessageListeners.cpp and include/Hooks.h - gated on CoMAP's own SKSE plugin version. This is why the 2026-08-27 compatibility pass recorded CoMAP as compatible: the integration is deliberate, not incidental. Also carries the rule 6 retroactive renumbering (a v1.0.10 was published before the pre-push hook existed to reject a patch component of 10; sorting 1.0.10 against 1.0.9 as strings puts them in the wrong order).
- failed to load in game - SKSE/Trampoline.h(287) displacement is out of range, confirmed by a real error dialog

## 1.0.0 - 2026-08-27 - working

### Changed
- git tag v1.0.0 pushed 2026-08-24

