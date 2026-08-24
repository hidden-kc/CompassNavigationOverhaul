# Port notes — INI-only settings → SKSE Menu Framework

**Version 1.0.0.** This is a fork of
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
