# DethKarz Comprehensive Modern Patch

This project is a comprehensive set of patches for the game DethKarz (Melbourne House, 1998) that aims to modernize the game. The patches are applied on startup without making modifications to the original executable. The following fixes/improvements are included:

- **Reworked Renderer.** The stock Glide renderer has been replaced with support for OpenGL.
- **Widescreen.** Adds full support for widescreen resolutions during gameplay. Menus are still rendered as 4:3.
- **High Refresh Rate.** Full support for high refresh rate displays has been added by allowing for higher or uncapped framerates.
- **Revamped LOD.** Cars no longer display at their lower LOD model when 30 feet in front of you.
- **Network stability.** In the original, the "Waiting on ..." box froze a race for good after a short
  connection hiccup, and a player whose game crashed froze everyone else. With the patch the race recovers on
  its own, and a disconnected player gets dropped.
- **Increased Tick Rate.** Multiplayer now works at a tick rate of 60Hz to mirror the game's original 60FPS target framerate. This was previously limited to 30Hz.
- **More players.** Network games take up to 12 players instead of 8 (16 with `MaxPlayers=16`), and player names can be up to 18 characters instead of 12.
- **Gamepads.** Support for modern controllers has been added by utilizing [SDL 2](https://libsdl.org).
- **Rumble.** Support for modern controllers also means support for rumble!
- **Relocated Saves** Settings, race records, saved games, ghosts and replays are kept in
  `%USERPROFILE%\Saved Games\Beam Software\DethKarz` instead of the install folder. Existing settings and savegames
  are copied over on first launch.
- **No registry.** The renderer choice and DragNet's network tuning, which the game originally was kept in the registry, are
  stored in `dkpatch.ini` instead.

It only supports the original build of `Dethkarz.exe` (built 16 Oct 1998, PE timestamp `0x362c0cfe`). This should be the version included in the GOG release.

## Install

Download one of the packages from the [releases](https://github.com/LANCommander/LANCommander.Patches.DethKarz/releases)
and extract it into the folder that holds `Dethkarz.exe`.

| package | contents | for |
|---|---|---|
| `dkpatch-<version>-dinput.zip` | `dinput.dll`, `dkpatch.ini`, `dkpatch\` | a plain install (the game loads `dinput.dll` from its folder) |
| `dkpatch-<version>-asi.zip` | `dkpatch.asi`, `dkpatch.ini`, `dkpatch\` | an ASI loader that is already installed (such as Ultimate ASI Loader) |
| `LANCommander.Interposer.Plugin.Dethkarz-<version>.zip` | `.interposer\Plugins\LANCommander.Interposer.Plugin.Dethkarz.dll`, `dkpatch\` | the [LANCommander Interposer](https://lancommander.app/Interposer) |

Multiplayer needs the Windows DirectPlay feature (*Turn Windows features on or off → Legacy Components →
DirectPlay*) and will require everyone to have these patches applied.

## Settings

Every option is on by default and described in [`dist/dkpatch.ini`](dist/dkpatch.ini). Set a key to `0` to turn
that fix off (`MaxPlayers=8` and `NameLength=12` give the original limits).

| section | key | what it does |
|---|---|---|
| `[Net]` | `Retries` | Resend requests before the "Waiting on ..." box opens (original 15 ≈ 2 s; default 40 ≈ 5 s) |
| | `ResendWhileWaiting` | Keeps resending while the box is open, so it closes when the connection comes back |
| | `HandlePlayerLoss` | Drops a player that DirectPlay reports as gone; clients leave cleanly if the host is gone |
| | `DirectPlayKeepAlive` | Host: lets DirectPlay notice a crashed or vanished player (after about 1.5 min) |
| | `FixClientSkipThresh` | Uses `ClientSkipThresh` as a number (the game read it as 0/1) |
| | `UseAuto`, `Ratio`, `Lead`, `Timeout`, `KeepAliveTimeOut`, `KeepAliveRetries`, `MinPacketSize`, `UseBumpUpEvents`, `ClientWait`, `ClientSkipThresh`, `UseFakeLatency`, `FakeLatencyMs` | DragNet's own tuning, formerly in the registry (`HKLM\SOFTWARE\Melbourne House\DethKarZ\Network`); the defaults are the game's. Host only |
| `[Widescreen]` | `Modes` | Adds 856x480 to the Glide resolution list (pick it in Options → Graphics) |
| | `HorPlusFov` | Keeps the 4:3 vertical view on wide screens and widens the horizontal one |
| | `ScaleHud` | Scales the HUD with the screen height |
| | `CenterMenus` | Shows menus, the pause menu and dialogs at 4:3 in the middle of the screen |
| | `MinimapScale` | Race minimap size in percent (default 140; 100 = original) |
| `[Video]` | `SkipIntro` | Skips the opening movie, whose forced 640×480×8 fullscreen mode many modern monitors reject at startup (default on) |
| | `SkipDeviceDialog` | No graphics device dialog at the first start: Glide (OpenGL) at the desktop resolution (default on; `-c` still opens it) |
| | `Device`, `DeviceWidth`, `DeviceHeight`, `DeviceBpp`, `DeviceData` | The chosen graphics device, written by the game (it kept it in the registry). Delete `Device` to be asked again |
| `[Graphics]` | `VehicleDrawDistance` | How far cars keep their detailed models, in percent of the original (default 400; `0` = always full detail). In the menu: the extra steps of the game's "car LOD" option (Customise Performance) |
| | `MenuOptions` | Options → Graphics shows MAX FRAMERATE in place of the game's frame rate option, plus the OpenGL settings (fullscreen, antialiasing, supersampling, texture filter); OK stores them in the settings |
| | `MaxFps` | Max framerate in races. The game always runs at 60 ticks per second (network games too: every player needs the patch), and frames drawn between the ticks are interpolated: `0` = the desktop refresh rate (default), `-1` = no limit, `60` = one frame per tick as the original, or a cap. Glide renderers only (OpenGL or nGlide) |
| | `ExperimentalTickRate` | Experimental, off by default (`0`; the older `TickRate` key is ignored): single-player races run the game itself at the refresh rate (`-1`) or a rate from 60 up, one real game step per frame. Changes how the game plays; turns `MaxFps` off |
| `[Players]` | `MaxPlayers` | Players per network game, 8..16 (default 12). Everyone needs the same value |
| | `NameLength` | Longest player name, 12..31 (default 18; longer names overlap the lobby's icons) |
| `[Saves]` | `Redirect` | Keep saves and settings in the Saved Games folder (default on; `0` = the original places) |
| | `Folder` | Use this folder instead (empty = `Saved Games\Beam Software\DethKarz`) |
| `[Gamepad]` | `UseSDL` | Gamepads through SDL (analog steering, trigger throttle, hotplug); wheels and other joysticks unchanged |
| | `Deadzone` | Left stick dead zone in percent (default 10; the game's own dead zone option applies too) |
| | `TriggerThrottle` | RT/LT accelerate/brake in a race; `0` = the left stick does, like a joystick |

## Building

Requires MSYS2 with the 32-bit MinGW toolchain and SDL 2 (`C:\msys64\mingw32`,
`pacman -S mingw-w64-i686-gcc mingw-w64-i686-SDL2`). SDL is linked statically, so the patch stays a single DLL
(SDL is under the [zlib license](https://github.com/libsdl-org/SDL/blob/SDL2/LICENSE.txt)).

```
git clone https://github.com/LANCommander/LANCommander.Patches.DethKarz.git
cd LANCommander.Patches.DethKarz
sh build.sh
```

Output:
- `build\dinput.dll`: the standalone patch (a `DirectInputCreateA` proxy that forwards to the system DLL).
- `build\dkpatch.asi`: the same patches as an ASI plugin.
- `build\LANCommander.Interposer.Plugin.Dethkarz.dll`: the Interposer plugin.
- `build\dkpatch\glide2x.dll`: the OpenGL renderer (a Glide 2 implementation on OpenGL 3.3).

`DK_VERSION=v1.2.3 sh build.sh` sets the version the patch writes to its log (`dev` otherwise).

GitHub Actions (`.github/workflows/build.yml`) builds every push and pull request. Pushing a semver tag
(`v1.2.3`, or `v1.2.3-rc.1` for a pre-release) builds that version and creates a draft release with the three
packages attached; publish it from the Releases page.

## Repository layout

| path | contents |
|---|---|
| `src/` | The patch: `dkpatch.c` (exe check, settings, guarded memory writes), `net.c`, `widescreen.c`, `players.c` (+ the generated `players_sites.h`), `gamepad.c`, `saves.c`, `opengl.c` + `glide_gl.c` (OpenGL renderer), `lod.c`, `tickrate.c`, `interp.c`, `gfxmenu.c`, `iat.c` (import table hooks); the loaders `dinput_proxy.c`, `asi.c`, `interposer_plugin.c`; plus a multiplayer test harness (`test.c`, `fakeddraw.c`, and `nullglide.c` for headless runs) that does nothing unless `DK_UNIT` is set |
| `dist/` | `dkpatch.ini`, shipped with the dinput.dll and ASI packages |

The game files (`Game\`, `bin\`) are not included. You need your own copy of the game.

## License

[MIT](LICENSE). Dethkarz itself is not included; you need your own copy of the game.
