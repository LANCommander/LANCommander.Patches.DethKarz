# LANCommander.Patches.DethKarz

A set of patches for the game DethKarz (Melbourne House, 1998) that are applied on startup without making modifications to the original executable. The patch attempts to fix the following issues:

- **Network stability.** In the original, the "Waiting on ..." box froze a race for good after a short
  connection hiccup, and a player whose game crashed froze everyone else. With the patch the race recovers on
  its own, and a player who is gone for good gets dropped.
- **Widescreen.** Adds an 856x480 Glide mode. The view is Hor+ (you see more at the sides), the HUD scales with
  the screen, and the menus stay 4:3 in the middle of the screen with black bars.

It only supports the original release of `Dethkarz.exe` (built 16 Oct 1998, PE timestamp `0x362c0cfe`).

## Install

1. Copy `dinput.dll` (from `build\` after building) and `dist\dkpatch.ini` into the folder that holds
   `Dethkarz.exe`.
2. Start the game. `dkpatch.log` in the same folder lists what was applied.

To uninstall, delete `dinput.dll` and `dkpatch.ini`.

Multiplayer needs the Windows DirectPlay feature (*Turn Windows features on or off → Legacy Components →
DirectPlay*). The network fixes work best when everyone has the patch. `Retries` and `DirectPlayKeepAlive`
only matter on the machine that hosts.

### LANCommander Interposer Plugin

The same patches also build as a [LANCommander Interposer](https://lancommander.app/Interposer)
plugin, `LANCommander.Interposer.Plugin.Dethkarz.dll`. Install either the plugin or the standalone `dinput.dll`,
not both. Its settings, the same keys as below, go under `Plugins.Dethkarz` in `.interposer\Config.yml`.

## Settings

Every option is on by default and described in [`dist/dkpatch.ini`](dist/dkpatch.ini). Set a key to `0` to turn
that fix off.

| section | key | what it does |
|---|---|---|
| `[Net]` | `Retries` | Resend requests before the "Waiting on ..." box opens (original 15 ≈ 2 s; default 40 ≈ 5 s) |
| | `ResendWhileWaiting` | Keeps resending while the box is open, so it closes when the connection comes back |
| | `HandlePlayerLoss` | Drops a player that DirectPlay reports as gone; clients leave cleanly if the host is gone |
| | `DirectPlayKeepAlive` | Host: lets DirectPlay notice a crashed or vanished player (after about 1.5 min) |
| | `FixClientSkipThresh` | Fixes a registry setting that was read as 0/1 instead of its value |
| `[Widescreen]` | `Modes` | Adds 856x480 to the Glide resolution list (pick it in Options → Graphics) |
| | `HorPlusFov` | Keeps the 4:3 vertical view on wide screens and widens the horizontal one |
| | `ScaleHud` | Scales the HUD with the screen height |
| | `CenterMenus` | Shows menus, the pause menu and dialogs at 4:3 in the middle of the screen |

Tested with the Glide renderer through nGlide 2.00. The Direct3D renderer's widescreen path hasn't been tested.

## Building

Requires MSYS2 with the 32-bit MinGW toolchain (`C:\msys64\mingw32`, `pacman -S mingw-w64-i686-gcc`).

```
git clone https://github.com/LANCommander/LANCommander.Patches.DethKarz.git
cd LANCommander.Patches.DethKarz
sh build.sh
```

Output:
- `build\dinput.dll`: the standalone patch (a `DirectInputCreateA` proxy that forwards to the system DLL).
- `build\LANCommander.Interposer.Plugin.Dethkarz.dll`: the Interposer plugin.

GitHub Actions (`.github/workflows/build.yml`) builds every push and pull request. Pushing a semver tag
(`v1.2.3`, or `v1.2.3-rc.1` for a pre-release) publishes a release with two zips: `dkpatch-<tag>.zip` (the
standalone patch) and `LANCommander.Interposer.Plugin.Dethkarz-<tag>.zip` (the plugin in its `.interposer\Plugins\`
folder).

## Repository layout

| path | contents |
|---|---|
| `src/` | The patch: `dkpatch.c` (exe check, settings, guarded memory writes), `net.c`, `widescreen.c`, `dinput_proxy.c`, `interposer_plugin.c`, plus a multiplayer test harness (`test.c`, `iat.c`) that does nothing unless `DK_UNIT` is set |
| `dist/` | `dkpatch.ini` and the Interposer files to ship |

The game files (`Game\`, `bin\`) are not included. You need your own copy of the game.