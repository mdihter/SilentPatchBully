SilentPatch for Bully: Scholarship Edition
60FPS fork - https://github.com/mdihter/SilentPatchBully
Originally created by Silent (CookiePLMonster) - https://github.com/CookiePLMonster/SilentPatchBully


DESCRIPTION

	This game, which shares a lot of the internals with GTA games, performs fairly well in its PC incarnation as is.
	However, it's more than likely that you have at some point spotted the amount of complaints Windows 10
	users have about the game, or maybe you have encountered crashes yourself.

	SilentPatch attempts to fix Bully memory management completely, so it behaves in the same way independent
	of Windows version. This is not the only fix included, however - most notably, it attempts to improve
	gameplay experience by improving frame pacing, as well as fixing a few other issues.

	This fork runs the game at a fixed 60FPS. Scripts known to misbehave at 60FPS (classes and a few missions)
	automatically lower the cap to 30FPS while they run, and 60FPS is restored the moment they end.

	Fixes featured in this plugin:

	CRASH AND BUG FIXES:
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

	QUALITY OF LIFE IMPROVEMENTS:
		* The game is declared DPI aware, so resolutions above 1080p are no longer magnified and cropped by
		  Windows display scaling - can be turned off with HighDPIAware=0
		* The game runs at 60FPS - scripts known to misbehave at that frame rate (classes and a few missions)
		  automatically lower the cap to 30FPS while they run and restore 60FPS afterwards, the list is
		  configurable in the INI file
		* Automatic settings configuration (1080p with High Shadows by default) - settings are written to the
		  game's registry settings whenever they change in the INI file, so changes made afterwards in the
		  game's own options menu are left alone
		* An experimental frame limiter mode (FrameLimiterSleep=1) that sleeps instead of spinning between
		  frames, freeing the CPU core the precise limiter otherwise keeps fully busy
		* An optional diagnostics log (LogFile=1) that records what was detected and applied, for bug reports
		* FILE_FLAG_NO_BUFFERING flag has been removed from IMG reading functions - potentially speeding
		  up streaming

	All options are documented in the comments of SilentPatchBully.ini, which has to sit next to SilentPatchBully.asi.


INSTALLATION

	Extract the archive contents to your Bully: Scholarship Edition directory, replacing any existing files.
	Make sure you check SilentPatchBully.ini before you try out the patch!

	The archive also contains:
		* dinput8.dll - Ultimate ASI Loader by ThirteenAG, needed for the game to load .asi plugins at all
		* MiniDumper.asi - writes a .dmp file to the game directory when the game crashes, for bug reports


SUBMITTING FEEDBACK

	If you want to report a bug (any feedback is very much appreciated), first ENSURE YOU HAVE AN UNMODDED GAME
	(texture mods are fine, scripts - not so much). Report it with the .dmp file MiniDumper created and a brief
	explanation of what you were doing when the game crashed, in the Issues page:

	https://github.com/mdihter/SilentPatchBully/issues

	Setting LogFile=1 in the INI file and attaching the resulting SilentPatchBully.log helps a lot.


CREDITS

	Silent (CookiePLMonster) (https://github.com/CookiePLMonster) - original author of SilentPatch
	P3ti (https://github.com/P3ti) - co-developer
	TroyWarez (https://github.com/TroyWarez) - co-developer
	amzy (https://www.twitch.tv/amzy) - testing, overall support


THIRD-PARTY COMPONENTS

	Ultimate ASI Loader (dinput8.dll) - ThirteenAG - MIT License
	https://github.com/ThirteenAG/Ultimate-ASI-Loader

	MiniDumper (MiniDumper.asi) - Silent (CookiePLMonster) - MIT License
	https://github.com/CookiePLMonster/MiniDumper
