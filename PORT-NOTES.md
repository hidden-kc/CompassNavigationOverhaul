# Port notes — INI-only settings → SKSE Menu Framework

**Version 1.0.3.** This is a fork of
[alexsylex/CompassNavigationOverhaul](https://github.com/alexsylex/CompassNavigationOverhaul)
2.2.0 that adds an in-game settings page driven by
[SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3), so the compass
and quest list can be configured while the game is running instead of only through
`CompassNavigationOverhaul.ini` at startup.

The upstream `README.md` and git history are untouched. This project's own versioning starts
at 1.0.0 (the standing default for this project), independent of upstream's 2.2.0.

## What changed from upstream

| File | Change |
| --- | --- |
| `include/SKSEMenuFramework.h` | Vendored, unmodified, from SKSE Menu Framework 3.13. Reaches the framework through `GetProcAddress`, so nothing has to be linked. |
| `include/UI.h`, `source/UI.cpp` | New. The settings page, the registration, and the live-apply entry point. |
| `include/Settings.h` | Added `Save()`, `RestoreDefaults()`, `Reload()` and `GetIniPath()` declarations; the settings themselves are unchanged from upstream. |
| `source/Settings.cpp` | Captures the compiled-in values as defaults before reading the INI, and can write or re-read every setting. Reads go through a null-safe helper rather than dereferencing the collection directly, and registration goes through a checked helper - both hardening moves ported over from Dragon's Eye Minimap's own port (and applied again for Local Map Upgrade), after the first of those crashed on startup once from exactly this class of bug. This codebase didn't have a live instance of that bug either - every setting's declared type already matched its name prefix - but the same fragile pattern (`INISettingCollection::GetSetting<T>` dereferencing a possibly-null pointer) is used here too, so the same protection was applied preemptively. |
| `source/MessageListeners.cpp` | Calls `UI::Register()` on `kPostPostLoad`, added alongside the existing `kPostLoad` branch that registers for Infinity UI and CoMAP compatibility messages. |
| `CMakeLists.txt` | Version bumped to this project's own 1.0.0. Auto-deploy copy is now conditional on the target game folder existing (`EXISTS` check added) - the original always ran the copy step and failed the build on a machine without that exact Steam install path. |
| `cmake/ports/commonlibsse-ng/portfile.cmake` | Fetches CommonLibVR (and its openvr submodule) over git instead of GitHub tarballs, same as Dragon's Eye Minimap's and Local Map Upgrade's forks - the pinned SHA512 for the same commit had already rotted (GitHub re-compresses generated tarballs over time) by the time this fork was set up. The pinned commit itself (`2b983f5281bfadd26ee20787390d2513e8ffe38a`) is unchanged - only how it's fetched. |

### What's live and what needs a restart

Every setting applies live except "Use metric units", which needs one explicit push: it's read
once by the compass at the moment its Scaleform instance is created/patched
(`Compass::SetUnits`), not on every frame, so the settings page calls that directly (handed to
the main thread, since Scaleform can't safely be touched from the framework's render callback)
whenever the checkbox changes, on Reload, and on Restore Defaults. Every other setting - marker
visibility, marker-detail timing, quest list position, its per-pace show delays, hide-in-combat
- is read directly from `settings::display::*` / `settings::questlist::*` at the point of use
every time (a marker draw, a quest-list `Invoke`), so a menu edit takes effect the next time
that code runs, no explicit apply call needed for those at all.

## 1.0.1: null-safety audit (preventative, no known crash in this mod)

Standing rule now (`CLAUDE.md` rule 14): every mod in this project gets audited for
unchecked null-dereference lookups, not just Dragon's Eye Minimap where the pattern actually
crashed. This mod's own INI settings (`utils::INISettingCollection`/`Settings.cpp`) were
already null-safe via the existing `Read<T>()` helper - no changes needed there, and no raw
`RE::INISettingCollection`/`RE::INIPrefSettingCollection`/`RE::GameSettingCollection` usage
exists anywhere in this codebase (the crash-causing pattern from the sibling mod simply
isn't present here). Real instances fixed elsewhere:

- `source/MessageListeners.cpp` (`InfinityUIMessageListener`) - `message->movie->GetMovieDef()
  ->GetFileURL()` dereferenced `movie` and `GetMovieDef()` unconditionally. Also, the
  `kPreReplaceInstance` branch used `CNO::Compass::GetSingleton()` right after
  `InitSingleton()` with no null check - inconsistent with the `kPostPatchInstance` branch a
  few lines down, which did check. Same fix applied to the `QuestItemList` singleton in the
  same file.
- `include/HUDMarkerManager.h`/`source/HUDMarkerManager.cpp` - `compass`/`questItemList` were
  cached as raw member pointers at construction and dereferenced unconditionally throughout
  `SetMarkersExtraInfo()`. If construction ever raced ahead of the Infinity UI messages that
  populate those singletons, `nullptr` would be cached forever and every subsequent frame
  would crash. Removed the caching; the singletons are now fetched fresh each call with a
  null check. Five faction lookups (`RE::TESForm::LookupByID(id)->As<RE::TESFaction>()`) were
  also dereferenced unconditionally - added a `LookupFaction()` helper with a null fallback,
  verified safe since the only uses compare the faction pointer by identity, never dereference it.
- `source/Hooks.cpp` - four `RE::TESObjectREFR::LookupByHandle(a_refHandle).get()` results
  (`UpdateQuests`, `UpdateLocations`, `UpdateEnemies`, `UpdatePlayerSetMarker`) were
  dereferenced unconditionally - a stale/unloaded ref handle returns null here.

No known crash was ever reported for any of these in this mod specifically - this pass is
preventative, following the same reasoning that caught the sibling mod's real bug.

## 1.0.2: attempted crash fix - real hardening, but not the actual cause

Confirmed by an actual in-game error dialog Liam hit on load: `SKSE/Trampoline.h(287):
displacement is out of range`. First diagnosis: `Hooks.h`'s `Install()` hooks two separate
call sites inside `HUDMarkerManager::UpdateLocations`
(`AllowedToShowMapMarkerHook::Address1()`/`Address2()`), both ultimately calling the same
C++ destination function, `AllowedToShowMapMarker`. The trampoline size calculation reserved
space for that hook only once, on the reasoning "the destination is the same, so allocate
for it once". Fixed by sizing for both `allowedToShowMapMarkerHook[0]`/`[1]` explicitly.

**This was real hardening (each hook site's `write_call()` is a distinct call, worth sizing
for explicitly) but turned out not to be the actual cause** - the crash recurred on 1.0.2,
still built and installed correctly. `SKSE::Trampoline::write_5branch()` deduplicates by
*destination address* internally (`_5branches`, a map from `a_dst` to an already-written
stub) - two hooks sharing a destination reuse the same stub automatically, no double
allocation ever happens. The 1.0.2 fix is harmless (a few extra bytes reserved, never used)
but was not what needed fixing. See 1.0.3 below for the real cause, found only after Liam
reported the crash persisted on 1.0.2 and pointed at real trampoline internals rather than
re-guessing.

## 1.0.3: the actual crash fix - CoMAP compatibility patch, unchecked failure paths

The real cause, found by reading `SKSE::Trampoline`'s own source: the crash is
`write_6branch()`, not `write_5branch()` - meaning the failing hook is `Hook<6>`, and the
only `Hook<6>` in this codebase is `compat::MapMarkerFramework::Install()`'s
`GetCompassMovieDefHook`, CoMAP's own compatibility patch (see "What's specific to..." /
`Case A` etc. above - `MessageListeners.cpp` installs this automatically when a plugin named
`MapMarkerFramework` under version 2.2.0 is detected).

Two unchecked failure paths in that one function, both now fixed:

1. `SigScanner::FindPattern<...>(a_moduleHandle)` returns `0` if the expected byte pattern
   isn't found in the installed CoMAP build (a version this pattern was never verified
   against). The old code did `+ 6` unconditionally, turning a "not found" into address `6`
   - hooking a bogus, essentially null address. Now checked; logs and skips the patch if the
   pattern isn't found.
2. `CustomTrampoline` (`include/utils/Trampoline.h`) searches backward from CoMAP's own
   module base for a free memory block within displacement range, to write the hook's
   trampoline stub into. **This search can genuinely fail** - confirmed in game, not
   hypothetical - especially in a process with many other DLLs loaded, leaving free blocks
   scarce near any one module. The old code called `inst->set_trampoline(base, a_size, ...)`
   with `base == nullptr` regardless, which left the trampoline reporting a non-zero
   *capacity* with a null backing pointer. The first `allocate()` through it then returned
   `nullptr` too, and `write_6branch()` computed a displacement of roughly
   `0 - (hook site address)` - nowhere close to `±2GB`, hence the crash. Fixed two ways:
   `CustomTrampoline` now leaves the underlying `SKSE::Trampoline` in its default *empty*
   state instead of calling `set_trampoline` on a null base, and a new `Trampoline::IsValid()`
   (`!inst->empty()`) lets `compat::MapMarkerFramework::Install()` check before writing -
   logging and skipping the patch cleanly instead of crashing if the search failed.

Net effect: on a system where this compatibility patch can't be safely installed, the mod
now runs completely normally without it - the patch was only ever for CoMAP versions
predating CoMAP's own 2.2.0 anyway, so skipping it doesn't lose current-CoMAP functionality.

## Building

Same as Dragon's Eye Minimap and Local Map Upgrade - Visual Studio (Desktop development with
C++) and vcpkg, neither pinned to a particular install location:

```
set VCPKG_ROOT=C:\path\to\vcpkg
configure.bat
build.bat
```

The DLL lands in `build/relwithdebinfo-se-only/CompassNavigationOverhaul.dll`. The configured
preset is SE/AE only; use `build-relwithdebinfo-all` for a build that also loads in Skyrim VR.

## Licence

Compass Navigation Overhaul is by alexsylex, MIT licensed. This fork keeps the original licence.
