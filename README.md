# SilentPatch for Bully: Scholarship Edition

This game, which shares a lot of the internals with GTA games, performs fairly well in its PC incarnation as is.
However, it's more than likely that you have at some point spotted the amount of complaints Windows 10
users have about the game, or maybe you have encountered crashes yourself.

SilentPatch attempts to fix Bully memory management completely, so it behaves in the same way independent
of Windows version. This is not the only fix included, however - most notably, it attempts to improve
gameplay experience by improving frame pacing, as well as fixing a few other issues.

Fixes featured in this plugin:

### Crash and bug fixes
* Collision loading code has been improved, fixing occasional crashes on initial game load
* Fixed game's objects pool usage, fixing possible crashes
* Fixed an occasional crash when starting Nutcrackin' or Music Class
* Fixed numerous instances of memory corruption on game exit
* Fixed an use-after-free in sound streaming code, causing a rare crash when talking to people
* Fixed handle leaks in audio code, preventing handles from accumulating during the game
* Fixed several memory leaks in audio code, preventing out of memory crashes during extended play sessions
* Made memory manager workarounds toggleable via the INI file - disabled by default, to be removed in the future
* Frame Limiter has been made much more precise and the game runs at a fixed 60FPS
  (as opposed to the stock 30FPS limiter being prone to dropping frames a lot)
* Fixed an issue where game would use more CPU than required when minimized

### Quality of life improvements
* The game runs at 60FPS - scripts known to misbehave at that frame rate (classes and a few missions)
  automatically lower the cap to 30FPS while they run and restore 60FPS afterwards, the list is configurable in the INI file
* Automatic settings configuration (1080p with High Shadows by default) - settings are written to the game's registry
  settings whenever they change in the INI file, so changes made afterwards in the game's own options menu are left alone
* An experimental frame limiter mode (`FrameLimiterSleep=1`) that sleeps instead of spinning between frames,
  freeing the CPU core the precise limiter otherwise keeps fully busy
* An optional diagnostics log (`LogFile=1`) that records what was detected and applied, for bug reports
* **FILE_FLAG_NO_BUFFERING** flag has been removed from IMG reading functions - potentially speeding up streaming

All options are documented in the comments of `SilentPatchBully.ini`, which has to sit next to `SilentPatchBully.asi`.

## Compilation requirements

The project builds with Visual Studio 2022 using the **Desktop development with C++** workload, which provides
the `v143` toolset and a Windows 10 SDK.

* The `ModUtils` dependency is a git submodule - after cloning, run `git submodule update --init` to fetch it,
  otherwise `Utils/MemoryMgr.h` will be missing.
* Building copies `SilentPatchBully.ini` next to the produced `SilentPatchBully.asi`.
* Every push is also built by GitHub Actions, and the resulting ASI and INI are available as workflow artifacts.
* Every push to `master` additionally publishes a GitHub Release with `SilentPatchBully.zip`: the Master build together with
  the Ultimate ASI Loader (`dinput8.dll`), MiniDumper and a ReadMe, ready to extract into the game directory.

## Submitting feedback

If you want to report it as a bug (any feedback is very much appreciated), first **ENSURE YOU HAVE AN UNMODDED GAME**
(texture mods are fine, scripts - not so much). You can report a bug (.dmp file + a brief explanation on what
you were doing when the game crashes) in the Issues page. Setting `LogFile=1` in the INI file and attaching the
resulting `SilentPatchBully.log` helps a lot.

## Credits

* [P3ti](https://github.com/P3ti) - co-developer
* [TroyWarez](https://github.com/TroyWarez) - co-developer
* [amzy](https://www.twitch.tv/amzy) - testing, overall support
