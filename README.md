# Motocross Madness 2 Modern Patch

This project is a set of patches for Motocross Madness 2 (Rainbow Studios / Microsoft, 2000) that makes the game
run properly on modern Windows. The patches are applied on startup without making modifications to the original
executable. The following fixes/improvements are included:

- **No compatibility mode.** The game no longer needs to run in Windows XP compatibility mode.
- **Movies that play, and no start-up crash.** The intro, the animated menu backgrounds and the credits are Indeo
  Video 5, a codec Windows has not shipped since Vista. The patch brings its own Indeo 5 decoder from FFmpeg.
- **Widescreen.** Races in any wide resolution with properly scaled HUD elements and correct geometry.
- **Less pop-in.** Trees and bushes stay 3-D models three times as far out as before. Other models keep their detailed versions twice as far.
- **High refresh rates.** Customizable maximum framerate with support for higher refresh rate displays.
- **No registry.** The game's settings (last profile, controller and 3-D card choice, quality switches) are now stored in config files and no longer reach the registry making the game fully portable.
- **Relocated saves.** Profiles, replays, ghosts and the crash log go to `%USERPROFILE%\Saved Games\Microsoft\Motocross Madness 2` instead of the install folder. Existing profiles and replays are copied over on the first start.
- **Gamepads.** Modern controllers (Xbox, PlayStation, Switch Pro, ...) through [SDL 2](https://libsdl.org), with
  analog steering and lean, and the triggers for throttle and brake.
- **Rumble.** The game's force feedback (impacts and the road under the wheels) drives the gamepad's rumble.
- **Fewer crashes.** A failed DirectSound buffer lock no longer crashes the engine sound.

It only supports `MCM2.EXE` 2.10 without SafeDisc (1,576,960 bytes, PE timestamp `0x63732172`). Any other executable is left alone and logged.

## Install

Download one of the packages from the [releases](https://github.com/LANCommander/LANCommander.Patches.MotocrossMadness2/releases)
and extract it into the folder that holds `MCM2.EXE`.

| package | contents | for |
|---|---|---|
| `mcm2patch-<version>-dinput.zip` | `dinput.dll`, `mcm2patch.ini`, `mcm2patch\` | a plain install (the game loads `dinput.dll` from its folder) |
| `mcm2patch-<version>-asi.zip` | `mcm2patch.asi`, `mcm2patch.ini`, `mcm2patch\` | an ASI loader that is already installed (such as Ultimate ASI Loader) |
| `LANCommander.Interposer.Plugin.MotocrossMadness2-<version>.zip` | `.interposer\Plugins\LANCommander.Interposer.Plugin.MotocrossMadness2.dll`, `mcm2patch\` | the [LANCommander Interposer](https://lancommander.app/Interposer) |

`mcm2patch\` holds the Indeo 5 movie decoder (`ir50.dll`). Remove the compatibility mode from `MCM2.EXE` if it was
set: it is no longer needed.

Multiplayer needs the Windows DirectPlay feature (*Turn Windows features on or off → Legacy Components →
DirectPlay*). Without it, Windows stops the game at start-up to offer installing DirectPlay.

## Settings

Every option is described in [`dist/mcm2patch.ini`](dist/mcm2patch.ini). Set a key to `0` to turn that fix off.
A `Settings.ini` in the save folder (`Saved Games\Microsoft\Motocross Madness 2`) with the same sections and keys
overrides `mcm2patch.ini` key by key (all but `[Saves]`, which says where that folder is), with the Interposer
plugin too.

| section | key | what it does |
|---|---|---|
| `[Compat]` | `WindowsVersion` | Reports Windows XP SP3 to the game and its DirectX components |
| | `MemoryStatus` | Reports at most 1 GB per memory figure (the game added them in 32 bits) |
| | `SoundLock` | A failed sound buffer lock no longer crashes the engine sound |
| `[Registry]` | `UseFile` | The game's registry settings in `[Game]` / `[Graphics]` instead of the registry |
| | `LogAccess` | Logs every registry access of the game |
| `[Saves]` | `Redirect` | Profiles, replays, ghosts and the crash log in the Saved Games folder |
| | `Folder` | Use this folder instead (empty = `Saved Games\Microsoft\Motocross Madness 2`) |
| `[Profile]` | `Name` | Create (from `ProfileTemplate\`) and load this profile at start |
| `[Video]` | `SkipDeviceDialog` | No 3-D card dialog: the first hardware device is used |
| | `Device` | Use this 3-D device instead (its number in the dialog, from 0; `-1` = automatic) |
| | `ConfigureDgVoodoo` | Sets the dgVoodoo 2 settings the game needs in `dgVoodoo.conf` (full screen, 4:3 pillarboxed, the wide modes listed; the original is kept as `dgVoodoo.conf.mcm2patch.bak`) |
| | `ClearScreen` | Clears each race frame to the fog colour first; the game clears only depth, so whatever the sky misses showed an older frame (flicker behind the HUD and in the sky) |
| `[Movies]` | `Decoder` | Play the Indeo 5 movies with the bundled decoder |
| `[Widescreen]` | `HorPlus` | 4:3 vertical view and more at the sides on wide resolutions |
| | `ScaleHud` | HUD and in-race dialogs scaled with the screen height, kept in their corners |
| `[Detail]` | `TreeDistance` | How far trees and bushes stay 3-D models instead of flat billboards, percent of the original (default 300; the original is 150 units on the top Ecosystem setting) |
| | `ModelDistance` | How far other models keep their detailed versions, percent of the original (default 200) |
| `[Framerate]` | `PreciseTimer` | Game clock and frame time steps from the performance counter |
| | `MaxFps` | `0` = the desktop refresh rate (default), `-1` = no cap, or a number |
| | `AiRate` | Computer riders decide how to steer and throttle at most this often per second (default 60; `0` = every frame). At high frame rates they otherwise weave and ride slowly |
| `[Gamepad]` | `UseSDL` | Gamepads through SDL, offered to the game as joysticks |
| | `Deadzone` | Left stick dead zone in percent (default 10) |
| | `Sensitivity` | Left stick steering response: `100` = linear, `50` = squared (default), `0` = cubed; full deflection is always full lock. The game's own cubing of joystick steering is compensated |
| | `Rumble`, `RumbleStrength`, `RumbleSustain` | Force feedback as rumble; strength, and how much of the steady road force is felt |
| | `TriggerButtons`, `DpadSteers` | Triggers also press buttons 0/1 (throttle/brake in the presets); the D-pad also steers |
| | `HideDuplicates`, `Subtype` | Hide a pad's own DirectInput entry; the device subtype reported (2 = joystick, analog steering) |
| | `Slots` | Empty gamepad slots offered for pads plugged in while the game runs (default 0) |
| | `StartKey`, `BackKey` | Keys Start and Back press (Escape, Backspace "back up") |
| `[Players]` | `MaxPlayers` | Stays 8 (more is not done yet) |
| `[Debug]` | `CrashLog` | Logs crashes with registers and a call chain to `mcm2patch.log` |

## Building

Requires MSYS2 with the 32-bit MinGW toolchain, SDL 2 and the FFmpeg build tools:

```
pacman -S mingw-w64-i686-gcc mingw-w64-i686-SDL2 make diffutils pkgconf
```

SDL is linked statically, so the patch stays a single DLL (SDL is under the
[zlib license](https://github.com/libsdl-org/SDL/blob/SDL2/LICENSE.txt)).

```
git clone https://github.com/LANCommander/LANCommander.Patches.MotocrossMadness2.git
cd LANCommander.Patches.MotocrossMadness2
sh build.sh
```

Output:
- `build\dinput.dll`: the standalone patch (a DirectInput proxy that forwards to the system DLL).
- `build\mcm2patch.asi`: the same patches as an ASI plugin.
- `build\LANCommander.Interposer.Plugin.MotocrossMadness2.dll`: the Interposer plugin.
- `build\mcm2patch\ir50.dll`: the Indeo 5 movie decoder, built by `build_ir50.sh`. That script downloads FFmpeg
  7.1.1 (checked against its SHA-256) and builds only the `indeo5` decoder. The decoder is a separate LGPL
  component (`src/ir50/README.md`).

`MCM_VERSION=v1.2.3 sh build.sh` sets the version the patch writes to its log (`dev` otherwise).

GitHub Actions (`.github/workflows/build.yml`) builds every push and pull request. Pushing a semver tag (`v1.2.3`, or
`v1.2.3-rc.1` for a pre-release) builds that version and creates a draft release with the three packages attached;
publish it from the Releases page.

## Repository layout

| path | contents |
|---|---|
| `src/` | The patch:<br>- `mcm2patch.c` (exe check, settings, guarded memory writes) and `iat.c` (import table hooks)<br>- `paths.c`, `compat.c`, `registry.c`, `saves.c`, `profile.c`, `device.c`, `movie.c`<br>- `d3d.c` (DirectDraw / Direct3D hooks), `widescreen.c`, `hud.c`, `dgvoodoo.c`, `framerate.c`<br>- `gamepad.c` + `pad_rumble.c`, `players.c` (research notes)<br>- `ir50/` (the Indeo 5 decoder)<br>- the loaders `dinput_proxy.c`, `asi.c`, `interposer_plugin.c`<br>- a test harness (`testharness.c`, scripted input and frame captures; does nothing unless `MCM_TEST` names a script) and `crashlog.c` |
| `dist/` | `mcm2patch.ini`, shipped with the dinput.dll and ASI packages |

The game files are not included. You need your own copy of the game.

## License

[MIT](LICENSE). The Indeo 5 decoder (`mcm2patch\ir50.dll`) is FFmpeg code under the LGPL 2.1 or later. Motocross Madness 2
itself is not included; you need your own copy of the game.
